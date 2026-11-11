/*
 * SPDX-FileCopyrightText: Copyright (c) 2019 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "utils.h"
#include <cuda.h>

#define ULL unsigned long long int

class ChannelDev {
  public:
    // --- comshu addition (diagnostic, opt-in) --------------------------------
    // Host-mapped counters so we can see exactly where the device stops. Set by
    // the backend after ChannelHost::init; NULL disables all of it. Slots:
    //   0 push entry      1 buffer-full seen   2 flusher branch  3 waiter branch
    //   4 flush() body    5 doorbell written   6 doorbell cleared 7 flush() done
    volatile unsigned long long* comshu_dbg = nullptr;
    __device__ __forceinline__ void comshu_bump(int slot) {
        if (comshu_dbg) atomicAdd((unsigned long long*)&comshu_dbg[slot], 1ull);
    }

  private:
    int id;
    volatile int* doorbell;

    uint8_t* buff;
    uint8_t* buff_end;

    /* head/tail pointers */
    uint8_t* volatile buff_write_head_ptr;
    uint8_t* volatile buff_write_tail_ptr;

  public:
    ChannelDev() {}

    __device__ __forceinline__ void push(void* packet, uint32_t nbytes) {
#if CUDA_VERSION >= 13010
        // assert(nbytes != 0);
#endif

        comshu_bump(0);
        uint8_t* curr_ptr = NULL;

        while (curr_ptr == NULL) {
            curr_ptr =
                (uint8_t*)atomicAdd((ULL*)&buff_write_head_ptr, (ULL)nbytes);

            /* if the current position plus nbytes is after buff_end, the
             * buffer is full.
             * Many warps could find condition true, but only the first warp
             * will find true the condition after. */
            if (curr_ptr + nbytes > buff_end) {
                comshu_bump(1);
                /* I am the first warp that found the buffer full and
                 * I am the one responsible for flushing the buffer out */
                if (curr_ptr <= buff_end) {
                    comshu_bump(2);
                    /* wait until everyone completed to write */
                    while (buff_write_tail_ptr != curr_ptr) {
                    }

                    /* flush buffer */
                    flush();
                } else {
                    comshu_bump(3);
                    /* waiting for buffer to flush */
                    while (buff_write_head_ptr > buff_end) {
                    }
                }
                curr_ptr = NULL;
            }
        }

        memcpy(curr_ptr, packet, nbytes);
        atomicAdd((ULL*)&buff_write_tail_ptr, (ULL)nbytes);
    }

    __device__ __forceinline__ void flush() {
        uint32_t nbytes = (uint32_t)(buff_write_tail_ptr - buff);
        // printf("FLUSH CHANNEL#%d: buffer bytes %d\n", id, nbytes);
        if (nbytes == 0) {
            return;
        }

        comshu_bump(4);
        /* make sure everything is visible in memory */
        __threadfence_system();

#if CUDA_VERSION >= 13010
        // assert(*doorbell == 0);
#endif
        /* notify current buffer has something*/
        *doorbell = nbytes;
        comshu_bump(5);

        /* wait for host to release the doorbell */
        while (*doorbell != 0)
            ;
        comshu_bump(6);

        /* reset head/tail
         *
         * comshu change (2026-07-27), correctness hardening — NOT a fix for the
         * case 07 hang. Upstream uses plain stores here, racing the
         * `atomicAdd((ULL*)&buff_write_head_ptr, nbytes)` that warps entering
         * push() issue on the same address; a plain store colliding with an
         * atomic RMW is a genuine lost update, so atomicExch is correct where a
         * store is not. Kept on those grounds alone.
         *
         * BOTH pointers must be reset, and tail before head (the fence between
         * them is upstream's and is load-bearing: a warp released by the head
         * reset must not observe the stale tail). An earlier revision of this
         * patch dropped the tail reset entirely, which wedges the channel after
         * the first flush — `nbytes = tail - buff` then grows without bound and
         * the next flusher's `while (tail != curr_ptr)` can never match.
         *
         * It does NOT resolve case 07. Measured: with a 1 GiB ring the hang
         * reproduces with full=0 flusher=0 flushbody=0 — the ring never fills,
         * this code never executes, and it still hangs. */
        atomicExch((ULL*)&buff_write_tail_ptr, (ULL)buff);
        __threadfence();
        atomicExch((ULL*)&buff_write_head_ptr, (ULL)buff);
        comshu_bump(7);

        // printf("FLUSH CHANNEL#%d: DONE\n", id);
    }

  private:
    /* called by the ChannelHost init */
    void init(int id, int* h_doorbell, int buff_size) {
        CUDA_SAFECALL(
            cudaHostGetDevicePointer((void**)&doorbell, (void*)h_doorbell, 0));

/* allocate large buffer */
#ifdef USE_ASYNC_STREAM
        CUDA_SAFECALL(cudaMalloc((void**)&buff, buff_size));
#else
        CUDA_SAFECALL(
            cudaHostAlloc((void**)&buff, buff_size, cudaHostAllocDefault));
#endif
        buff_write_head_ptr = buff;
        buff_write_tail_ptr = buff;
        buff_end = buff + buff_size;
        this->id = id;
    }

    friend class ChannelHost;
};

class ChannelHost {
  private:
    volatile int* doorbell;

    cudaStream_t stream;
    ChannelDev* ch_dev;

    /* pointers to device buffer */
    uint8_t* dev_buff_read_head;
    uint8_t* dev_buff;

    /* receiving thread */
    pthread_t thread;
    volatile bool thread_started;

  public:
    int id;
    int buff_size;

  public:
    ChannelHost() {}

    void init(int id, int buff_size, ChannelDev* ch_dev,
              void* (*thread_fun)(void*), void* args = NULL) {
        this->buff_size = buff_size;
        this->id = id;
        /* get device properties */
        cudaDeviceProp prop;
        int device = 0;
        cudaGetDeviceProperties(&prop, device);
        if (prop.canMapHostMemory == 0) {
            CUDA_SAFECALL(cudaSetDeviceFlags(cudaDeviceMapHost));
        }

#ifdef USE_ASYNC_STREAM
        /* create stream that will read memory with highest possible priority */
        int priority_high, priority_low;
        CUDA_SAFECALL(
            cudaDeviceGetStreamPriorityRange(&priority_low, &priority_high));
        CUDA_SAFECALL(cudaStreamCreateWithPriority(
            &stream, cudaStreamNonBlocking, priority_high));
#endif

        /* create doorbell */
        CUDA_SAFECALL(
            cudaHostAlloc((void**)&doorbell, sizeof(int), cudaHostAllocMapped));
        /* set doorbell to zero */
        *doorbell = 0;

        /* initialize device channel */
        this->ch_dev = ch_dev;
        ch_dev->init(id, (int*)doorbell, buff_size);

        dev_buff = ch_dev->buff;
        dev_buff_read_head = dev_buff;
        if (thread_fun != NULL) {
            thread_started = true;
            pthread_create(&thread, NULL, (void* (*)(void*))thread_fun, args);
        } else {
            thread_started = false;
        }
    }

    /* when used in nvbit we don't want to dealloc because
     * when modules are unloaded the driver automatically
     * deallocates CUDA malloc, so further deallocs done
     * here will result in errors */
    void destroy(bool dealloc) {
        if (thread_started) {
            thread_started = false;
            pthread_join(thread, NULL);
        }
        if (dealloc) {
            CUDA_SAFECALL(cudaFreeHost((int*)doorbell));
#ifdef USE_ASYNC_STREAM
            CUDA_SAFECALL(cudaStreamDestroy(stream));
            CUDA_SAFECALL(cudaFree(ch_dev->buff));
#else
            CUDA_SAFECALL(cudaFreeHost(ch_dev->buff));
#endif

        }
    }

    bool is_active() { return thread_started; }

    uint32_t recv(void* buff, uint32_t max_buff_size) {
        assert(max_buff_size > 0);
        assert(doorbell != NULL);
        uint32_t buff_nbytes = *doorbell;
        if (buff_nbytes == 0) {
            return 0;
        }
        int nbytes = buff_nbytes;

        if (buff_nbytes > max_buff_size) {
            nbytes = max_buff_size;
        }
#ifdef USE_ASYNC_STREAM
        CUDA_SAFECALL(cudaMemcpyAsync(buff, dev_buff_read_head, nbytes,
                                      cudaMemcpyDeviceToHost, stream));
        CUDA_SAFECALL(cudaStreamSynchronize(stream));
#else
        memcpy(buff, dev_buff_read_head, nbytes);
#endif
        int bytes_left = buff_nbytes - nbytes;
        assert(bytes_left >= 0);
        if (bytes_left > 0) {
            dev_buff_read_head += nbytes;
        } else {
            dev_buff_read_head = dev_buff;
        }

        *doorbell = bytes_left;
        // printf("HOST RECEIVED nbytes %d - bytes left %d\n", nbytes,
        // bytes_left);
        return nbytes;
    }

    pthread_t get_thread() { return thread; }

    friend class MultiChannelHost;
};

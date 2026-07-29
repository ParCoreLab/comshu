# Record-format benchmark

Raw-POD `.pcs` vs FlatBuffers, using the real record types. Results and the
recommendation live in `plans/PLAN-20260729-1830-record-format-flatbuffers-*.md`.

    git clone --depth 1 https://github.com/google/flatbuffers.git fb
    g++ -std=c++20 -O2 -I ../../libpcomm/include -I fb/include bench.cpp -o bench
    g++ -std=c++20 -O2 -I ../../libpcomm/include -I ../../providers/ibverbs \
        -I fb/include ibbench.cpp -o ibbench
    ./bench /tmp && ./ibbench /tmp

`ibcost.sbatch` measures the denominator that matters — untraced vs traced
`ib_send_bw` message rate on real hardware. Without it the serialization numbers
are unanchored: the write path turns out to be under 4% of tracing cost, so
comparing 12 ns against 37 ns in isolation invites the wrong conclusion.

#pragma once
// Public-UCP records (600-699). These describe application/communication
// intent. They deliberately do not claim that raw UCP count is a byte count.

#include <cstdint>

#include "pcomm/core/record.hpp"
#include "pcomm/core/vocab.hpp"

namespace pcomm::providers::ucx::records {
using namespace pcomm::core;

enum class StatusClass : std::uint8_t {
    Immediate = 0,  // nullptr / UCS_OK
    Error = 1,      // UCS_PTR_IS_ERR
    Request = 2,    // a real request pointer
};

PCOMM_RECORD(UcxContext, RecordKind::Entity, 600, "ucx_context") {
    Handle context;
    std::uint32_t api_major;
    std::uint32_t api_minor;
};

PCOMM_RECORD(UcxWorker, RecordKind::Entity, 602, "ucx_worker") {
    Handle worker;
    Handle context;
};

PCOMM_RECORD(UcxEndpoint, RecordKind::Entity, 604, "ucx_endpoint") {
    Handle endpoint;
    Handle worker;
};

PCOMM_RECORD(UcxTagSend, RecordKind::Event, 606, "ucx_tag_send") {
    Handle        endpoint;
    Handle        buffer;
    Handle        request;
    std::uint64_t count;  // raw UCP datatype count, not universally bytes
    std::uint64_t tag;
    std::int32_t  status;        // UCS_OK or the decoded error status
    std::uint8_t  status_class;  // StatusClass
};

}  // namespace pcomm::providers::ucx::records

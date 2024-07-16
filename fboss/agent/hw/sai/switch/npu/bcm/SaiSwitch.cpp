// Copyright 2004-present Facebook. All Rights Reserved.

#include "fboss/agent/hw/sai/switch/SaiSwitch.h"

#include "fboss/agent/hw/sai/api/TamApi.h"

#include <folly/String.h>

extern "C" {
#if !defined(BRCM_SAI_SDK_XGS_AND_DNX)
#include <experimental/saiexperimentalswitch.h>
#include <experimental/saitamextensions.h>
#else
#ifndef IS_OSS_BRCM_SAI
#include <experimental/saiswitchextensions.h>
#else
#include <saiswitchextensions.h>
#endif
#endif
}

namespace {
std::string eventName(uint32_t eventID) {
  switch (eventID) {
    case SAI_SWITCH_EVENT_TYPE_STABLE_FULL:
      return "SAI_SWITCH_EVENT_TYPE_STABLE_FULL";
    case SAI_SWITCH_EVENT_TYPE_STABLE_ERROR:
      return "SAI_SWITCH_EVENT_TYPE_STABLE_ERROR";
    case SAI_SWITCH_EVENT_TYPE_UNCONTROLLED_SHUTDOWN:
      return "SAI_SWITCH_EVENT_TYPE_UNCONTROLLED_SHUTDOWN";
    case SAI_SWITCH_EVENT_TYPE_WARM_BOOT_DOWNGRADE:
      return "SAI_SWITCH_EVENT_TYPE_UNCONTROLLED_SHUTDOWN";
    case SAI_SWITCH_EVENT_TYPE_PARITY_ERROR:
      return "SAI_SWITCH_EVENT_TYPE_PARITY_ERROR";
#if defined BRCM_SAI_SDK_GTE_11_0
    case SAI_SWITCH_EVENT_TYPE_INTERRUPT:
      return "SAI_SWITCH_EVENT_TYPE_INTERRUPT";
#endif
  }
  return folly::to<std::string>("unknown event type: ", eventID);
}

std::string correctionType(sai_switch_correction_type_t type) {
  switch (type) {
    case SAI_SWITCH_CORRECTION_TYPE_NO_ACTION:
      return "SAI_SWITCH_CORRECTION_TYPE_NO_ACTION";
    case SAI_SWITCH_CORRECTION_TYPE_FAIL_TO_CORRECT:
      return "SAI_SWITCH_CORRECTION_TYPE_FAIL_TO_CORRECT";
    case SAI_SWITCH_CORRECTION_TYPE_ENTRY_CLEAR:
      return "SAI_SWITCH_CORRECTION_TYPE_ENTRY_CLEAR";
    case SAI_SWITCH_CORRECTION_TYPE_CACHE_RESTORE:
      return "SAI_SWITCH_CORRECTION_TYPE_CACHE_RESTORE";
    case SAI_SWITCH_CORRECTION_TYPE_HW_CACHE_RESTORE:
      return "SAI_SWITCH_CORRECTION_TYPE_HW_CACHE_RESTORE";
    case SAI_SWITCH_CORRECTION_TYPE_SPECIAL:
      return "SAI_SWITCH_CORRECTION_TYPE_SPECIAL";
    case SAI_SWITCH_CORRECTION_TYPE_ALL:
      return "SAI_SWITCH_CORRECTION_TYPE_ALL";
  }
  return "correction-type-unknown";
}

std::string errorType(sai_switch_error_type_t type) {
  switch (type) {
    case SAI_SWITCH_ERROR_TYPE_UNKNOWN:
      return "SAI_SWITCH_ERROR_TYPE_UNKNOWN";
    case SAI_SWITCH_ERROR_TYPE_PARITY:
      return "SAI_SWITCH_ERROR_TYPE_PARITY";
    case SAI_SWITCH_ERROR_TYPE_ECC_SINGLE_BIT:
      return "SAI_SWITCH_ERROR_TYPE_ECC_SINGLE_BIT";
    case SAI_SWITCH_ERROR_TYPE_ECC_DOUBLE_BIT:
      return "SAI_SWITCH_ERROR_TYPE_ECC_DOUBLE_BIT";
#if defined BRCM_SAI_SDK_GTE_11_0
      // IRE Errors
    case SAI_SWITCH_ERROR_TYPE_IRE_ECC:
      return "SAI_SWITCH_ERROR_TYPE_IRE_ECC";
    case SAI_SWITCH_ERROR_TYPE_IRE_RCY_INTERFACE:
      return "SAI_SWITCH_ERROR_TYPE_IRE_RCY_INTERFACE";
    case SAI_SWITCH_ERROR_TYPE_IRE_INTERNAL_INTERFACE:
      return "SAI_SWITCH_ERROR_TYPE_IRE_INTERNAL_INTERFACE";
    case SAI_SWITCH_ERROR_TYPE_IRE_NIF:
      return "SAI_SWITCH_ERROR_TYPE_IRE_NIF";
    case SAI_SWITCH_ERROR_TYPE_IRE_UNEXPECTED_SOP:
      return "SAI_SWITCH_ERROR_TYPE_IRE_UNEXPECTED_SOP";
    case SAI_SWITCH_ERROR_TYPE_IRE_UNEXPECTED_MOP:
      return "SAI_SWITCH_ERROR_TYPE_IRE_UNEXPECTED_MOP";
    case SAI_SWITCH_ERROR_TYPE_IRE_NEGATIVE_DELTA:
      return "SAI_SWITCH_ERROR_TYPE_IRE_NEGATIVE_DELTA";
    case SAI_SWITCH_ERROR_TYPE_IRE_INCOMPLETE_WORD:
      return "SAI_SWITCH_ERROR_TYPE_IRE_INCOMPLETE_WORD";
    case SAI_SWITCH_ERROR_TYPE_IRE_BAD_REASSEMBLY_CONTEXT:
      return "SAI_SWITCH_ERROR_TYPE_IRE_BAD_REASSEMBLY_CONTEXT";
    case SAI_SWITCH_ERROR_TYPE_IRE_INVALID_REASSEMBLY_CONTEXT:
      return "SAI_SWITCH_ERROR_TYPE_IRE_INVALID_REASSEMBLY_CONTEXT";
    case SAI_SWITCH_ERROR_TYPE_IRE_TDM_DOC_NAME_0:
      return "SAI_SWITCH_ERROR_TYPE_IRE_TDM_DOC_NAME_0";
    case SAI_SWITCH_ERROR_TYPE_IRE_TDM_DOC_NAME_1:
      return "SAI_SWITCH_ERROR_TYPE_IRE_TDM_DOC_NAME_1";
    case SAI_SWITCH_ERROR_TYPE_IRE_TDM_DOC_NAME_2:
      return "SAI_SWITCH_ERROR_TYPE_IRE_TDM_DOC_NAME_2";
    case SAI_SWITCH_ERROR_TYPE_IRE_TDM_DOC_NAME_3:
      return "SAI_SWITCH_ERROR_TYPE_IRE_TDM_DOC_NAME_3";
    case SAI_SWITCH_ERROR_TYPE_IRE_REASSEMBLY_CONTEXT:
      return "SAI_SWITCH_ERROR_TYPE_IRE_REASSEMBLY_CONTEXT";
    case SAI_SWITCH_ERROR_TYPE_IRE_BYTE_NUM:
      return "SAI_SWITCH_ERROR_TYPE_IRE_BYTE_NUM";
    case SAI_SWITCH_ERROR_TYPE_IRE_TIMEOUT:
      return "SAI_SWITCH_ERROR_TYPE_IRE_TIMEOUT";
    case SAI_SWITCH_ERROR_TYPE_IRE_REASSEMBLY:
      return "SAI_SWITCH_ERROR_TYPE_IRE_REASSEMBLY";
    case SAI_SWITCH_ERROR_TYPE_IRE_FIFO:
      return "SAI_SWITCH_ERROR_TYPE_IRE_FIFO";
    case SAI_SWITCH_ERROR_TYPE_IRE_DATA_PATH_CRC:
      return "SAI_SWITCH_ERROR_TYPE_IRE_DATA_PATH_CRC";
      // ITPP Errors
    case SAI_SWITCH_ERROR_TYPE_ITPP_ECC:
      return "SAI_SWITCH_ERROR_TYPE_ITPP_ECC";
    case SAI_SWITCH_ERROR_TYPE_ITPP_PSIZE_TYPE_0_MISMATCH:
      return "SAI_SWITCH_ERROR_TYPE_ITPP_PSIZE_TYPE_0_MISMATCH";
    case SAI_SWITCH_ERROR_TYPE_ITPP_PSIZE_TYPE_1_MISMATCH:
      return "SAI_SWITCH_ERROR_TYPE_ITPP_PSIZE_TYPE_1_MISMATCH";
    case SAI_SWITCH_ERROR_TYPE_ITPP_PSIZE_TYPE_2_MISMATCH:
      return "SAI_SWITCH_ERROR_TYPE_ITPP_PSIZE_TYPE_2_MISMATCH";
    case SAI_SWITCH_ERROR_TYPE_ITPP_PSIZE_TYPE_3_MISMATCH:
      return "SAI_SWITCH_ERROR_TYPE_ITPP_PSIZE_TYPE_3_MISMATCH";
    case SAI_SWITCH_ERROR_TYPE_ITPP_PSIZE_TYPE_4_MISMATCH:
      return "SAI_SWITCH_ERROR_TYPE_ITPP_PSIZE_TYPE_4_MISMATCH";
    case SAI_SWITCH_ERROR_TYPE_ITPPD_ECC:
      return "SAI_SWITCH_ERROR_TYPE_ITPPD_ECC";
    case SAI_SWITCH_ERROR_TYPE_ITPPD_PSIZE_TYPE_0_MISMATCH:
      return "SAI_SWITCH_ERROR_TYPE_ITPPD_PSIZE_TYPE_0_MISMATCH";
    case SAI_SWITCH_ERROR_TYPE_ITPPD_PSIZE_TYPE_1_MISMATCH:
      return "SAI_SWITCH_ERROR_TYPE_ITPPD_PSIZE_TYPE_1_MISMATCH";
    case SAI_SWITCH_ERROR_TYPE_ITPPD_PSIZE_TYPE_2_MISMATCH:
      return "SAI_SWITCH_ERROR_TYPE_ITPPD_PSIZE_TYPE_2_MISMATCH";
    case SAI_SWITCH_ERROR_TYPE_ITPPD_PSIZE_TYPE_3_MISMATCH:
      return "SAI_SWITCH_ERROR_TYPE_ITPPD_PSIZE_TYPE_3_MISMATCH";
    case SAI_SWITCH_ERROR_TYPE_ITPPD_PSIZE_TYPE_4_MISMATCH:
      return "SAI_SWITCH_ERROR_TYPE_ITPPD_PSIZE_TYPE_4_MISMATCH";
      // EPNI errors
    case SAI_SWITCH_ERROR_TYPE_EPNI_ERROR_ECC:
      return "SAI_SWITCH_ERROR_TYPE_EPNI_ERROR_ECC";
    case SAI_SWITCH_ERROR_TYPE_EPNI_POST_MIRR_OVF_INT:
      return "SAI_SWITCH_ERROR_TYPE_EPNI_POST_MIRR_OVF_INT";
    case SAI_SWITCH_ERROR_TYPE_EPNI_RCYM_OVF_INT:
      return "SAI_SWITCH_ERROR_TYPE_EPNI_RCYM_OVF_INT";
    case SAI_SWITCH_ERROR_TYPE_EPNI_POST_MIRR_DROP_INT:
      return "SAI_SWITCH_ERROR_TYPE_EPNI_POST_MIRR_DROP_INT";
    case SAI_SWITCH_ERROR_TYPE_EPNI_IRE_WFQ_SAT_INT:
      return "SAI_SWITCH_ERROR_TYPE_EPNI_IRE_WFQ_SAT_INT";
    case SAI_SWITCH_ERROR_TYPE_EPNI_IRE_WFQ_UDP_INT:
      return "SAI_SWITCH_ERROR_TYPE_EPNI_IRE_WFQ_UDP_INT";
    case SAI_SWITCH_ERROR_TYPE_EPNI_FIFO_UNDERFLOW_INT:
      return "SAI_SWITCH_ERROR_TYPE_EPNI_FIFO_UNDERFLOW_INT";
    case SAI_SWITCH_ERROR_TYPE_EPNI_FIFO_OVERFLOW_INT:
      return "SAI_SWITCH_ERROR_TYPE_EPNI_FIFO_OVERFLOW_INT";
    case SAI_SWITCH_ERROR_TYPE_EPNI_RXI_OVERFLOW_INT:
      return "SAI_SWITCH_ERROR_TYPE_EPNI_RXI_OVERFLOW_INT";
    case SAI_SWITCH_ERROR_TYPE_EPNI_FWD_APPEND_HEADER_INT:
      return "SAI_SWITCH_ERROR_TYPE_EPNI_FWD_APPEND_HEADER_INT";
    case SAI_SWITCH_ERROR_TYPE_EPNI_RCY_APPEND_HEADER_INT:
      return "SAI_SWITCH_ERROR_TYPE_EPNI_RCY_APPEND_HEADER_INT";
    case SAI_SWITCH_ERROR_TYPE_EPNI_SMALL_PACKET_HEADER_STRIP:
      return "SAI_SWITCH_ERROR_TYPE_EPNI_SMALL_PACKET_HEADER_STRIP";
    case SAI_SWITCH_ERROR_TYPE_EPNI_ILLEGAL_IFC_NUMBER_INT:
      return "SAI_SWITCH_ERROR_TYPE_EPNI_ILLEGAL_IFC_NUMBER_INT";
    case SAI_SWITCH_ERROR_TYPE_EPNI_VISIBILITY_PACKET_ENDED:
      return "SAI_SWITCH_ERROR_TYPE_EPNI_VISIBILITY_PACKET_ENDED";
    case SAI_SWITCH_ERROR_TYPE_EPNI_ECC_ECC_1B_ERR_INT:
      return "SAI_SWITCH_ERROR_TYPE_EPNI_ECC_ECC_1B_ERR_INT";
    case SAI_SWITCH_ERROR_TYPE_EPNI_ECC_ECC_2B_ERR_INT:
      return "SAI_SWITCH_ERROR_TYPE_EPNI_ECC_ECC_2B_ERR_INT";
      // Aligner errors
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_ERROR_ECC:
      return "SAI_SWITCH_ERROR_TYPE_ALIGNER_ERROR_ECC";
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_ETPP_INC_ABOVE_TH_INT:
      return "SAI_SWITCH_ERROR_TYPE_ALIGNER_ETPP_INC_ABOVE_TH_INT";
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_ETPP_SOP_DEC_ABOVE_TH_INT:
      return "SAI_SWITCH_ERROR_TYPE_ALIGNER_ETPP_SOP_DEC_ABOVE_TH_INT";
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_ETPP_EOP_DEC_ABOVE_TH_INT:
      return "SAI_SWITCH_ERROR_TYPE_ALIGNER_ETPP_EOP_DEC_ABOVE_TH_INT";
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_ETPP_TAIL_ABOVE_INC_INT:
      return "SAI_SWITCH_ERROR_TYPE_ALIGNER_ETPP_TAIL_ABOVE_INC_INT";
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_ETPP_TOTAL_DEC_ABOVE_PKT_SIZE_INT:
      return "SAI_SWITCH_ERROR_TYPE_ALIGNER_ETPP_TOTAL_DEC_ABOVE_PKT_SIZE_INT";
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_ETPP_PKT_INC_ABOVE_MAX_PKT_SIZE_INT:
      return "SAI_SWITCH_ERROR_TYPE_ALIGNER_ETPP_PKT_INC_ABOVE_MAX_PKT_SIZE_INT";
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_ETPP_TAIL_ABOVE_TH_INT:
      return "SAI_SWITCH_ERROR_TYPE_ALIGNER_ETPP_TAIL_ABOVE_TH_INT";
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_PKT_SIZE_EOP_MISMATCH_INT:
      return "SAI_SWITCH_ERROR_TYPE_ALIGNER_PKT_SIZE_EOP_MISMATCH_INT";
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_ETPP_TOO_SMALL_AFTER_EDIT:
      return "SAI_SWITCH_ERROR_TYPE_ALIGNER_ETPP_TOO_SMALL_AFTER_EDIT";
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_SMALL_CROPPED_MIRR_HEADER:
      return "SAI_SWITCH_ERROR_TYPE_ALIGNER_SMALL_CROPPED_MIRR_HEADER";
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_FIFO_UNDERFLOW_INT:
      return "SAI_SWITCH_ERROR_TYPE_ALIGNER_FIFO_UNDERFLOW_INT";
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_FIFO_OVERFLOW_INT:
      return "SAI_SWITCH_ERROR_TYPE_ALIGNER_FIFO_OVERFLOW_INT";
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_ECC_ECC_1B_ERR_INT:
      return "SAI_SWITCH_ERROR_TYPE_ALIGNER_ECC_ECC_1B_ERR_INT";
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_ECC_ECC_2B_ERR_INT:
      return "SAI_SWITCH_ERROR_TYPE_ALIGNER_ECC_ECC_2B_ERR_INT";
    // FQP errors
    case SAI_SWITCH_ERROR_TYPE_FQP_ERROR_ECC:
      return "SAI_SWITCH_ERROR_TYPE_FQP_ERROR_ECC";
    case SAI_SWITCH_ERROR_TYPE_FQP_TXQ_OVF_INT:
      return "SAI_SWITCH_ERROR_TYPE_FQP_TXQ_OVF_INT";
    case SAI_SWITCH_ERROR_TYPE_FQP_TXQ_READ_CONJESTED_INT:
      return "SAI_SWITCH_ERROR_TYPE_FQP_TXQ_READ_CONJESTED_INT";
    case SAI_SWITCH_ERROR_TYPE_FQP_TXQ_WRITE_CONJESTED_INT:
      return "SAI_SWITCH_ERROR_TYPE_FQP_TXQ_WRITE_CONGESTED_INT";
    case SAI_SWITCH_ERROR_TYPE_FQP_ECC_ECC_1B_ERR_INT:
      return "SAI_SWITCH_ERROR_TYPE_FQP_ECC_ECC_1B_ERR_INT";
    case SAI_SWITCH_ERROR_TYPE_FQP_ECC_ECC_2B_ERR_INT:
      return "SAI_SWITCH_ERROR_TYPE_FQP_ECC_ECC_2B_ERR_INT";

    // All reassembly context taken
    case SAI_SWITCH_ERROR_TYPE_RQP_PACKET_REASSEMBLY_RCM_ALL_CONTEXTS_TAKEN_ERR:
      return "SAI_SWITCH_ERROR_TYPE_RQP_PACKET_REASSEMBLY_RCM_ALL_CONTEXTS_TAKEN_ERR";
    case SAI_SWITCH_ERROR_TYPE_RQP_PACKET_REASSEMBLY_RCM_ALL_CONTEXTS_TAKEN_DISCARD_ERR:
      return "SAI_SWITCH_ERROR_TYPE_RQP_PACKET_REASSEMBLY_RCM_ALL_CONTEXTS_TAKEN_DISCARD_ERR";
    case SAI_SWITCH_ERROR_TYPE_RTP_TABLE_CHANGE:
      return "SAI_SWITCH_ERROR_TYPE_RTP_TABLE_CHANGE";
#endif
    default:
      break;
  }
  return folly::sformat("Unknown error type: {} ", static_cast<int>(type));
}

#if defined BRCM_SAI_SDK_GTE_11_0
bool isIreErrorType(sai_switch_error_type_t type) {
  switch (type) {
    case SAI_SWITCH_ERROR_TYPE_IRE_ECC:
    case SAI_SWITCH_ERROR_TYPE_IRE_RCY_INTERFACE:
    case SAI_SWITCH_ERROR_TYPE_IRE_INTERNAL_INTERFACE:
    case SAI_SWITCH_ERROR_TYPE_IRE_NIF:
    case SAI_SWITCH_ERROR_TYPE_IRE_UNEXPECTED_SOP:
    case SAI_SWITCH_ERROR_TYPE_IRE_UNEXPECTED_MOP:
    case SAI_SWITCH_ERROR_TYPE_IRE_NEGATIVE_DELTA:
    case SAI_SWITCH_ERROR_TYPE_IRE_INCOMPLETE_WORD:
    case SAI_SWITCH_ERROR_TYPE_IRE_BAD_REASSEMBLY_CONTEXT:
    case SAI_SWITCH_ERROR_TYPE_IRE_INVALID_REASSEMBLY_CONTEXT:
    case SAI_SWITCH_ERROR_TYPE_IRE_TDM_DOC_NAME_1:
    case SAI_SWITCH_ERROR_TYPE_IRE_TDM_DOC_NAME_2:
    case SAI_SWITCH_ERROR_TYPE_IRE_TDM_DOC_NAME_3:
    case SAI_SWITCH_ERROR_TYPE_IRE_REASSEMBLY_CONTEXT:
    case SAI_SWITCH_ERROR_TYPE_IRE_BYTE_NUM:
    case SAI_SWITCH_ERROR_TYPE_IRE_TIMEOUT:
    case SAI_SWITCH_ERROR_TYPE_IRE_REASSEMBLY:
    case SAI_SWITCH_ERROR_TYPE_IRE_FIFO:
    case SAI_SWITCH_ERROR_TYPE_IRE_DATA_PATH_CRC:
      return true;
    default:
      break;
  }
  return false;
}

bool isItppError(sai_switch_error_type_t type) {
  switch (type) {
    case SAI_SWITCH_ERROR_TYPE_ITPP_ECC:
    case SAI_SWITCH_ERROR_TYPE_ITPP_PSIZE_TYPE_0_MISMATCH:
    case SAI_SWITCH_ERROR_TYPE_ITPP_PSIZE_TYPE_1_MISMATCH:
    case SAI_SWITCH_ERROR_TYPE_ITPP_PSIZE_TYPE_2_MISMATCH:
    case SAI_SWITCH_ERROR_TYPE_ITPP_PSIZE_TYPE_3_MISMATCH:
    case SAI_SWITCH_ERROR_TYPE_ITPP_PSIZE_TYPE_4_MISMATCH:
    case SAI_SWITCH_ERROR_TYPE_ITPPD_ECC:
    case SAI_SWITCH_ERROR_TYPE_ITPPD_PSIZE_TYPE_0_MISMATCH:
    case SAI_SWITCH_ERROR_TYPE_ITPPD_PSIZE_TYPE_1_MISMATCH:
    case SAI_SWITCH_ERROR_TYPE_ITPPD_PSIZE_TYPE_2_MISMATCH:
    case SAI_SWITCH_ERROR_TYPE_ITPPD_PSIZE_TYPE_3_MISMATCH:
    case SAI_SWITCH_ERROR_TYPE_ITPPD_PSIZE_TYPE_4_MISMATCH:
      return true;
    default:
      break;
  }
  return false;
}

bool isEpniError(sai_switch_error_type_t type) {
  switch (type) {
    case SAI_SWITCH_ERROR_TYPE_EPNI_ERROR_ECC:
    case SAI_SWITCH_ERROR_TYPE_EPNI_POST_MIRR_OVF_INT:
    case SAI_SWITCH_ERROR_TYPE_EPNI_RCYM_OVF_INT:
    case SAI_SWITCH_ERROR_TYPE_EPNI_POST_MIRR_DROP_INT:
    case SAI_SWITCH_ERROR_TYPE_EPNI_IRE_WFQ_SAT_INT:
    case SAI_SWITCH_ERROR_TYPE_EPNI_IRE_WFQ_UDP_INT:
    case SAI_SWITCH_ERROR_TYPE_EPNI_FIFO_UNDERFLOW_INT:
    case SAI_SWITCH_ERROR_TYPE_EPNI_FIFO_OVERFLOW_INT:
    case SAI_SWITCH_ERROR_TYPE_EPNI_RXI_OVERFLOW_INT:
    case SAI_SWITCH_ERROR_TYPE_EPNI_FWD_APPEND_HEADER_INT:
    case SAI_SWITCH_ERROR_TYPE_EPNI_RCY_APPEND_HEADER_INT:
    case SAI_SWITCH_ERROR_TYPE_EPNI_SMALL_PACKET_HEADER_STRIP:
    case SAI_SWITCH_ERROR_TYPE_EPNI_ILLEGAL_IFC_NUMBER_INT:
    case SAI_SWITCH_ERROR_TYPE_EPNI_VISIBILITY_PACKET_ENDED:
    case SAI_SWITCH_ERROR_TYPE_EPNI_ECC_ECC_1B_ERR_INT:
    case SAI_SWITCH_ERROR_TYPE_EPNI_ECC_ECC_2B_ERR_INT:
      return true;
    default:
      break;
  }
  return false;
}

bool isAlignerErrorType(sai_switch_error_type_t type) {
  switch (type) {
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_ERROR_ECC:
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_ETPP_INC_ABOVE_TH_INT:
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_ETPP_SOP_DEC_ABOVE_TH_INT:
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_ETPP_EOP_DEC_ABOVE_TH_INT:
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_ETPP_TAIL_ABOVE_INC_INT:
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_ETPP_TOTAL_DEC_ABOVE_PKT_SIZE_INT:
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_ETPP_PKT_INC_ABOVE_MAX_PKT_SIZE_INT:
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_ETPP_TAIL_ABOVE_TH_INT:
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_PKT_SIZE_EOP_MISMATCH_INT:
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_ETPP_TOO_SMALL_AFTER_EDIT:
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_SMALL_CROPPED_MIRR_HEADER:
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_FIFO_UNDERFLOW_INT:
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_FIFO_OVERFLOW_INT:
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_ECC_ECC_1B_ERR_INT:
    case SAI_SWITCH_ERROR_TYPE_ALIGNER_ECC_ECC_2B_ERR_INT:
      return true;
    default:
      break;
  }
  return false;
}

bool isFqpError(sai_switch_error_type_t type) {
  switch (type) {
    case SAI_SWITCH_ERROR_TYPE_FQP_ERROR_ECC:
    case SAI_SWITCH_ERROR_TYPE_FQP_TXQ_OVF_INT:
    case SAI_SWITCH_ERROR_TYPE_FQP_TXQ_READ_CONJESTED_INT:
    case SAI_SWITCH_ERROR_TYPE_FQP_TXQ_WRITE_CONJESTED_INT:
    case SAI_SWITCH_ERROR_TYPE_FQP_ECC_ECC_1B_ERR_INT:
    case SAI_SWITCH_ERROR_TYPE_FQP_ECC_ECC_2B_ERR_INT:
      return true;
    default:
      break;
  }
  return false;
}

bool allReassemblyContextsTakenError(sai_switch_error_type_t type) {
  switch (type) {
    case SAI_SWITCH_ERROR_TYPE_RQP_PACKET_REASSEMBLY_RCM_ALL_CONTEXTS_TAKEN_ERR:
    case SAI_SWITCH_ERROR_TYPE_RQP_PACKET_REASSEMBLY_RCM_ALL_CONTEXTS_TAKEN_DISCARD_ERR:
      return true;
    default:
      break;
  }
  return false;
}

bool rtpTableChangedEvent(sai_switch_error_type_t type) {
  if (type == SAI_SWITCH_ERROR_TYPE_RTP_TABLE_CHANGE) {
    return true;
  } else {
    return false;
  }
}

#endif

} // namespace

namespace facebook::fboss {

void SaiSwitch::switchEventCallback(
    sai_size_t /*buffer_size*/,
    const void* buffer,
    uint32_t event_type) {
  const sai_switch_ser_log_info_t* eventInfo =
      static_cast<const sai_switch_ser_log_info_t*>(buffer);
  std::stringstream sstream;
  sstream << "received switch event: " << eventName(event_type)
          << ", event info(";
  bool correctible = true;
  if (eventInfo) {
    sstream << "correction type=" << correctionType(eventInfo->correction_type)
            << ", flags=" << std::hex << eventInfo->flags;
    correctible =
        (eventInfo->correction_type !=
         SAI_SWITCH_CORRECTION_TYPE_FAIL_TO_CORRECT);
    sstream << ", error type=" << errorType(eventInfo->error_type);
  }
  sstream << ")";
  XLOG(WARNING) << sstream.str();
  switch (event_type) {
    case SAI_SWITCH_EVENT_TYPE_STABLE_FULL:
    case SAI_SWITCH_EVENT_TYPE_STABLE_ERROR:
    case SAI_SWITCH_EVENT_TYPE_UNCONTROLLED_SHUTDOWN:
    case SAI_SWITCH_EVENT_TYPE_WARM_BOOT_DOWNGRADE:
      getSwitchStats()->asicError();
      break;
    case SAI_SWITCH_EVENT_TYPE_PARITY_ERROR:
      if (correctible) {
        getSwitchStats()->corrParityError();
      } else {
        getSwitchStats()->uncorrParityError();
      }
      break;
#if defined BRCM_SAI_SDK_GTE_11_0
    case SAI_SWITCH_EVENT_TYPE_INTERRUPT: {
      auto ireError = isIreErrorType(eventInfo->error_type);
      auto itppError = isItppError(eventInfo->error_type);
      auto epniError = isEpniError(eventInfo->error_type);
      auto alignerError = isAlignerErrorType(eventInfo->error_type);
      auto fqpError = isFqpError(eventInfo->error_type);
      auto allReassemblyContextsTaken =
          allReassemblyContextsTakenError(eventInfo->error_type);
      auto rtpTableChanged = rtpTableChangedEvent(eventInfo->error_type);
      XLOG(ERR) << " Got interrupt event, is IRE: " << ireError
                << " is ITPP: " << itppError << " is EPNI: " << epniError
                << " is Aligner: " << alignerError << " is FQP: " << fqpError
                << " all reassembly context taken: "
                << allReassemblyContextsTaken
                << " rtp table changed: " << rtpTableChanged;
      if (ireError) {
        getSwitchStats()->ireError();
      } else if (itppError) {
        getSwitchStats()->itppError();
      } else if (epniError) {
        getSwitchStats()->epniError();
      } else if (alignerError) {
        getSwitchStats()->alignerError();
      } else if (fqpError) {
        getSwitchStats()->forwardingQueueProcessorError();
      } else if (allReassemblyContextsTaken) {
        getSwitchStats()->allReassemblyContextsTaken();
      } else if (rtpTableChanged) {
        // RTP table change notification is vendor specific, this
        // means a change in switch reachability over fabric!
        // Initiate processing for RTP table change, ie. invoke
        // the common switchReachabilityChange handling.
        switchReachabilityChangeTopHalf();
        getSwitchStats()->switchReachabilityChangeCount();
      }
    } break;
#endif
  }
}

void SaiSwitch::tamEventCallback(
    sai_object_id_t /*tam_event_id*/,
    sai_size_t /*buffer_size*/,
    const void* /*buffer*/,
    uint32_t /*attr_count*/,
    const sai_attribute_t* /*attr_list*/) {
  // no-op
}

} // namespace facebook::fboss

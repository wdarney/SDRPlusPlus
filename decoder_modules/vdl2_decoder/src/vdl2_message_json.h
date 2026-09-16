#pragma once
#include "vdl2_dsp.h"

// Called at the decoder boundary after formatted_text is complete.
// No fields are extracted from formatted_text.
void populateMessageJSON(VDL2Message& msg, const VDL2ProtocolDecoder::Result& decoded,
                         bool hasAVLC, const std::string& direction = "");

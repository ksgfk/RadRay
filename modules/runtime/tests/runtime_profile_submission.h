#pragma once

#include "runtime_profile_support.h"

#include <radray/runtime/frame_submission.h>

namespace radray::profile {

struct SubmissionObservation {
    uint64_t FrameSerial{0}, SubmitCallbackNs{0}, CompletionCallbackNs{0};
    uint32_t FlightIndex{0};
    uint32_t SampleIndex{0};
    bool Available{false}, CompletionSucceeded{false};
    bool TraceEnabled{true};
};

// The frame row stays at a fixed address until the host drains every flight.
// These are host callback entry times, after Submit returned / the fence was observed.
template <typename Result>
void ObserveSubmission(Result& result, uint64_t serial, uint32_t flight, SubmissionObservation& observation) {
    if constexpr (requires { result.Submission->Serial(); result.Submission->OnSubmitted; result.Submission->OnCompleted; }) {
        if (!result.Submission || result.Submission->Serial() != serial) return;
        observation.FrameSerial = serial;
        observation.FlightIndex = flight;
        observation.Available = true;
        auto& submission = *result.Submission;
        submission.OnSubmitted = [prior = std::move(submission.OnSubmitted), target = &observation, serial, flight] {
            const auto now = TimestampNs();
            if (target->TraceEnabled) TraceMarker(target->SampleIndex, flight, serial, "submit");
            if (prior) prior();
            if (target->FrameSerial == serial && target->FlightIndex == flight) target->SubmitCallbackNs = now;
        };
        submission.OnCompleted = [prior = std::move(submission.OnCompleted), target = &observation, serial, flight](bool success) {
            const auto now = TimestampNs();
            if (target->TraceEnabled) TraceMarker(target->SampleIndex, flight, serial, "complete");
            if (prior) prior(success);
            if (target->FrameSerial == serial && target->FlightIndex == flight) {
                target->CompletionCallbackNs = now;
                target->CompletionSucceeded = success;
            }
        };
    }
}

}  // namespace radray::profile

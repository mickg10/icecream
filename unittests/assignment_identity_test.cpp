#include "comm.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool value, const char *message)
{
    if (!value) {
        std::cerr << "assignment_identity_test: " << message << '\n';
        std::exit(1);
    }
}

void test_assignment_transfer()
{
    constexpr uint64_t epoch = UINT64_C(0x1020304050607080);
    constexpr uint64_t nonce = UINT64_C(0x9080706050403020);
    constexpr uint64_t c_guid = UINT64_C(0x1122334455667788);
    UseCSMsg assigned("x86_64", "worker", 8765, 17, true, 3, 0,
                      epoch, nonce, c_guid, UINT64_C(0));
    CompileJob job;
    require(assigned.applyAssignmentTo(&job),
            "complete UseCS assignment was rejected");
    require(job.jobID() == 17 && job.assignmentEpoch() == epoch &&
                job.assignmentNonce() == nonce && job.hasAssignmentIdentity(),
            "UseCS assignment did not reach CompileJob");
    require(job.cGuid() == c_guid && job.tuSeq() == 0 &&
                job.hasCompileIdentity(),
            "UseCS C_GUID/TU_SEQ did not reach CompileJob");

    UseCSMsg partial("x86_64", "worker", 8765, 18, true, 3, 0, epoch, 0);
    CompileJob rejected;
    require(!partial.applyAssignmentTo(&rejected),
            "partial assignment identity was accepted");

    NoCSMsg no_cs(20, 3, epoch, nonce, c_guid, UINT64_C(1));
    require(no_cs.assignmentEpoch() == epoch && no_cs.assignmentNonce() == nonce &&
                no_cs.cGuid() == c_guid && no_cs.tuSeq() == 1,
            "NoCS identity did not round-trip");
}

void test_result_transfer()
{
    constexpr uint64_t epoch = UINT64_C(41);
    constexpr uint64_t nonce = UINT64_C(9);
    CompileResultMsg result;
    result.setAssignmentIdentity(epoch, nonce);
    CompileJob job;
    job.setJobID(17);
    job.setAssignmentIdentity(epoch, nonce);
    job.setCompileIdentity(UINT64_C(7), UINT64_C(9));
    result.setCompileIdentity(UINT64_C(7), UINT64_C(9));
    require(result.compileIdentityMatches(job),
            "CompileResult C_GUID/TU_SEQ did not match");
    result.setCompileIdentity(UINT64_C(0), UINT64_C(0));
    require(!result.compileIdentityMatches(job),
            "omitted C_GUID was accepted");
    result.setCompileIdentity(UINT64_C(7), UINT64_C(0));
    require(!result.compileIdentityMatches(job),
            "mismatched TU_SEQ was accepted");

    JobDoneMsg done(17, 0, JobDoneMsg::FROM_SERVER, 1, epoch, nonce,
                    UINT64_C(7), UINT64_C(9));
    require(done.assignmentEpoch() == epoch && done.assignmentNonce() == nonce &&
                done.cGuid() == 7 && done.tuSeq() == 9,
            "JobDone identity did not round-trip");
}

} // namespace

int main()
{
    test_assignment_transfer();
    test_result_transfer();
    std::cout << "assignment_identity_test: PASS\n";
    return 0;
}

/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
#include "siblingpin.h"

#include "job.h"

#include <list>
#include <utility>

void pin_sibling_environments(Job *master, const SiblingPinSelection &selection)
{
    if (!master || master->masterJobFor().empty()
            || selection.selected_platform.empty()
            || selection.selected_environment.empty()) {
        return;
    }

#ifdef ICECC_TEST_SIBLING_PIN_MUTANT_EXACT_WORKER
    /* Exact historical defect: discard the compatible platform selected by
       envs_match()/can_install() and rewrite siblings to the worker platform. */
    const std::string &platform = selection.worker_platform;
#else
    const std::string &platform = selection.selected_platform;
#endif
    const std::pair<std::string, std::string> selected =
        std::make_pair(platform, selection.selected_environment);
    const std::list<Job *> siblings = master->masterJobFor();
    for (Job * const sibling : siblings) {
        sibling->clearEnvironments();
        sibling->appendEnvironment(selected);
    }
}

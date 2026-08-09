/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
#ifndef SIBLINGPIN_H
#define SIBLINGPIN_H

#include <string>

class Job;

/* The two platform identities in one dispatch are deliberately distinct:

     selected_platform  platform of the environment matched for the client;
     worker_platform    physical platform advertised by the compile server.

   A compatible dispatch may select i686 for an x86_64 worker.  Keeping both
   names in the value passed to the pinning transition makes it impossible to
   silently replace the selected client environment with the worker's exact
   platform without crossing this single, unit-tested boundary. */
struct SiblingPinSelection
{
    std::string selected_platform;
    std::string selected_environment;
    std::string worker_platform;
};

void pin_sibling_environments(Job *master, const SiblingPinSelection &selection);

#endif

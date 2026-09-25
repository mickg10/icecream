// Regressions for the client's cached `clang -dumpmachine` probe: the cache is
// keyed by invocation name (target-prefixed links to one binary differ), and a
// replaced compiler binary or a new or edited config file next to it is probed again.
#include "client.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace std;

static string dir;

// Installs a fake compiler via rename (a new inode, like a package upgrade).
// It counts its runs and answers by its invocation name, as clang does, unless
// a clang.cfg next to it sets the target.
static void install_compiler(const string &answer_for_plain_name)
{
    const string path = dir + "/real-clang", tmp = path + ".new";
    ofstream(tmp) << "#!/bin/sh\necho >> '" << dir << "/runs'\n"
                  << "[ -s '" << dir << "/clang.cfg' ] && exec sed 's/^--target=//' '" << dir << "/clang.cfg'\n"
                  << "case \"$0\" in *aarch64-linux-gnu-*) echo aarch64-unknown-linux-gnu ;;"
                  << " *) echo " << answer_for_plain_name << " ;; esac\n";
    if (chmod(tmp.c_str(), 0755) != 0 || rename(tmp.c_str(), path.c_str()) != 0)
        exit(2);
}

static int runs()
{
    ifstream in(dir + "/runs");
    int n = 0;
    for (string line; getline(in, line);)
        ++n;
    return n;
}

static void expect(const char *what, const string &name, const string &want, int want_runs)
{
    setenv("ICECC_CXX", (dir + "/" + name).c_str(), 1);
    CompileJob job;
    job.setLanguage(CompileJob::Lang_CXX);
    for (int round = 0; round < 2; ++round) { // the second round must be a cache hit
        const string got = clang_get_default_target(job);
        if (got != want) {
            cerr << what << ": got '" << got << "', expected '" << want << "'\n";
            exit(1);
        }
    }
    if (runs() != want_runs) {
        cerr << what << ": compiler ran " << runs() << " times, expected " << want_runs << "\n";
        exit(1);
    }
}

int main()
{
    char tmpl[] = "/tmp/dumpmachinecache.XXXXXX";
    if (!mkdtemp(tmpl))
        return 2;
    dir = tmpl;
    setenv("TMPDIR", tmpl, 1);
    unsetenv("ICECC_CC");
    install_compiler("x86_64-pc-linux-gnu");
    if (symlink("real-clang", (dir + "/clang").c_str()) != 0 ||
        symlink("real-clang", (dir + "/aarch64-linux-gnu-clang").c_str()) != 0)
        return 2;

    expect("plain name", "clang", "x86_64-pc-linux-gnu", 1);
    expect("target-prefixed name", "aarch64-linux-gnu-clang", "aarch64-unknown-linux-gnu", 2);
    expect("plain name, cached", "clang", "x86_64-pc-linux-gnu", 2);
    install_compiler("riscv64-unknown-linux-gnu");
    expect("replaced compiler", "clang", "riscv64-unknown-linux-gnu", 3);
    ofstream(dir + "/clang.cfg") << "--target=armv7-unknown-linux-gnueabihf\n";
    expect("new config file", "clang", "armv7-unknown-linux-gnueabihf", 4);
    // Same inode; a new size, since two writes can share a coarse mtime.
    ofstream(dir + "/clang.cfg") << "--target=powerpc64le-linux-gnu\n";
    expect("edited config file", "clang", "powerpc64le-linux-gnu", 5);

    return system(("rm -rf '" + dir + "'").c_str()) == 0 ? 0 : 2;
}

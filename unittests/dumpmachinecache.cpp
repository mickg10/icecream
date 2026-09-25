// Regressions for the client's cached `clang -dumpmachine` probe: the cache is
// keyed by invocation name (target-prefixed links to one binary differ), and a
// replaced compiler binary, or a config file or directory created, edited or
// deleted next to it or in the user/system config directories it names in -v, is
// probed again.  An @include in a tracked config file, a config override in the
// environment or an incomplete -v answer bypasses the cache.
#include "client.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace std;

static string dir;

// Installs a fake compiler via rename (a new inode, like a package upgrade).  It
// counts its runs, prints a clang -v block naming its config directories on
// stderr, and answers by its invocation name, as clang does, unless a config file
// sets the target: for the prefixed name user/aarch64-linux-gnu-clang.cfg, then
// for any name user/clang.cfg, sys/clang.cfg or the clang.cfg next to it.
static void install_compiler(const string &answer_for_plain_name)
{
    const string path = dir + "/real-clang", tmp = path + ".new";
    ofstream(tmp) << "#!/bin/sh\necho >> '" << dir << "/runs'\n"
                  << "[ \"$1\" = -v ] && { echo 'clang version fake'; echo 'InstalledDir: " << dir << "';"
                  << " echo 'System configuration file directory: " << dir << "/sys';"
                  << " echo 'User configuration file directory: " << dir << "/user'; exit 0; } >&2\n"
                  << "case \"$0\" in *aarch64-linux-gnu-*) p='" << dir << "/user/aarch64-linux-gnu-clang.cfg' ;;"
                  << " *) p= ;; esac\n"
                  << "for c in $p '" << dir << "/user/clang.cfg' '" << dir << "/sys/clang.cfg' '" << dir << "/clang.cfg'; do\n"
                  << "  [ -s \"$c\" ] && exec sed 's/^--target=//' \"$c\"\ndone\n"
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

// Two lookups; unless the cache is bypassed, the second must be a hit.
static void expect(const char *what, const string &name, const string &want, int want_runs)
{
    setenv("ICECC_CXX", (dir + "/" + name).c_str(), 1);
    CompileJob job;
    job.setLanguage(CompileJob::Lang_CXX);
    for (int round = 0; round < 2; ++round) {
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
    unsetenv("CLANG_NO_DEFAULT_CONFIG");
    unsetenv("CCC_OVERRIDE_OPTIONS");
    install_compiler("x86_64-pc-linux-gnu");
    if (symlink("real-clang", (dir + "/clang").c_str()) != 0 ||
        symlink("real-clang", (dir + "/aarch64-linux-gnu-clang").c_str()) != 0)
        return 2;

    // A miss runs the compiler twice (-v, then -dumpmachine); a hit not at all.
    expect("plain name", "clang", "x86_64-pc-linux-gnu", 2);
    expect("target-prefixed name", "aarch64-linux-gnu-clang", "aarch64-unknown-linux-gnu", 4);
    expect("plain name, cached", "clang", "x86_64-pc-linux-gnu", 4);
    install_compiler("riscv64-unknown-linux-gnu");
    expect("replaced compiler", "clang", "riscv64-unknown-linux-gnu", 6);
    ofstream(dir + "/clang.cfg") << "--target=armv7-unknown-linux-gnueabihf\n";
    expect("new config file", "clang", "armv7-unknown-linux-gnueabihf", 8);
    // Same inode; a new size, since two writes can share a coarse mtime.
    ofstream(dir + "/clang.cfg") << "--target=powerpc64le-linux-gnu\n";
    expect("edited config file", "clang", "powerpc64le-linux-gnu", 10);

    // The named directories did not exist: creating them, even empty, is a miss.
    if (mkdir((dir + "/sys").c_str(), 0700) != 0 || mkdir((dir + "/user").c_str(), 0700) != 0)
        return 2;
    expect("new empty config directories", "clang", "powerpc64le-linux-gnu", 12);
    ofstream(dir + "/sys/clang.cfg") << "--target=s390x-ibm-linux-gnu\n";
    expect("new system config file", "clang", "s390x-ibm-linux-gnu", 14);
    ofstream(dir + "/user/clang.cfg") << "--target=mips64el-linux-gnuabi64\n";
    expect("new user config file", "clang", "mips64el-linux-gnuabi64", 16);
    ofstream(dir + "/user/clang.cfg") << "--target=sparc64-linux-gnu\n";
    expect("edited user config file", "clang", "sparc64-linux-gnu", 18);
    // A config file for one invocation name changes the directory for every name.
    ofstream(dir + "/user/aarch64-linux-gnu-clang.cfg") << "--target=aarch64-custom-linux-gnu\n";
    expect("new invocation-specific config file", "aarch64-linux-gnu-clang", "aarch64-custom-linux-gnu", 20);
    expect("other name after a new file in its user directory", "clang", "sparc64-linux-gnu", 22);
    if (unlink((dir + "/user/clang.cfg").c_str()) != 0)
        return 2;
    expect("deleted user config file", "clang", "s390x-ibm-linux-gnu", 24);
    if (unlink((dir + "/user/aarch64-linux-gnu-clang.cfg").c_str()) != 0 || rmdir((dir + "/user").c_str()) != 0)
        return 2;
    expect("deleted user config directory", "clang", "s390x-ibm-linux-gnu", 26);
    expect("deleted directory, prefixed name", "aarch64-linux-gnu-clang", "s390x-ibm-linux-gnu", 28);

    // A tracked config file with an @include is never cached; once it is gone, the directory is
    // as before and so is the record written then.
    ofstream(dir + "/sys/other.cfg") << "@common.cfg\n";
    expect("config file with an @include", "clang", "s390x-ibm-linux-gnu", 32);
    if (unlink((dir + "/sys/other.cfg").c_str()) != 0)
        return 2;
    expect("@include gone, the earlier record holds again", "clang", "s390x-ibm-linux-gnu", 32);

    // An environment config override probes every time and leaves the record alone.
    setenv("CLANG_NO_DEFAULT_CONFIG", "1", 1);
    expect("config override in the environment", "clang", "s390x-ibm-linux-gnu", 34);
    unsetenv("CLANG_NO_DEFAULT_CONFIG");
    expect("after the override, cached", "clang", "s390x-ibm-linux-gnu", 34);

    // No clang -v block: the config directories are unknown, so nothing is cached.
    const string odd = dir + "/odd-clang";
    ofstream(odd) << "#!/bin/sh\necho >> '" << dir << "/runs'\n"
                  << "[ \"$1\" = -v ] && { echo 'gcc version 13.2.0 (fake)' >&2; exit 0; }\n"
                  << "echo x86_64-odd-linux-gnu\n";
    if (chmod(odd.c_str(), 0755) != 0)
        return 2;
    expect("incomplete -v answer", "odd-clang", "x86_64-odd-linux-gnu", 38);

    return system(("rm -rf '" + dir + "'").c_str()) == 0 ? 0 : 2;
}

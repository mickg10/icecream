/* env_install_check.h -- a NAMED warning for env installs whose
 * character-device entries could not be extracted (silent in stock).
 *
 * The environment tarball may carry ./dev/null as a character device.
 * Extraction needs CAP_MKNOD, which the unprivileged daemon never has;
 * libarchive reports the failed mknod as a WARNING, which the classic
 * r < ARCHIVE_WARN loop tolerated WITHOUT A WORD -- the env installed
 * and nobody knew the device was missing.
 *
 * With the std-fd fix in place a chroot without /dev/null no longer
 * breaks the compiler (the daemon re-opens std fds from the host fs and
 * workit never execs with them closed), so a missing device node is NOT
 * fatal -- but it must not be SILENT either: the extract child logs a
 * named warning when it happens.
 *
 * chardev_extract_is_complete() is that check: the node must EXIST and
 * BE a character device at its target path.  Header-only, so the
 * daemon's extract child and a unit test call the same code.
 */
#ifndef ICECC_ENV_INSTALL_CHECK_H
#define ICECC_ENV_INSTALL_CHECK_H

#include <sys/stat.h>
#include <sys/types.h>

namespace icecc {

/* True iff path exists and is a character device -- i.e. the mknod that
   libarchive attempted actually landed.  A regular file or a missing
   path means the extraction FAILED (no CAP_MKNOD, or the mknod was
   masked); the caller must treat the environment as broken, not install
   it. */
inline bool chardev_extract_is_complete(const char *path) noexcept
{
    if (path == nullptr)
        return false;
    struct stat info;
    if (::stat(path, &info) != 0)
        return false;
    return S_ISCHR(info.st_mode);
}

}  // namespace icecc

#endif

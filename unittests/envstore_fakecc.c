/* Static multi-call stand-in for the binaries of a compiler environment,
   used by envstore-run.sh.  Built -static so it runs inside the env chroot
   without any libraries.

   - "true" and "as": exit 0 (verify_env's bin/true; the as probe).
   - anything else ("gcc", ...): a fake compiler.  It reads the
     preprocessed source from stdin, writes /tmp/fakecc.running (so the test
     can see a job running INSIDE this tree), waits while /tmp/hold exists,
     then writes "FAKEOBJ\n" to the -o file.  The marker lets the test tell
     a remote compile in this env from a local fallback (a real object).  */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    if (strcmp(base, "true") == 0 || strcmp(base, "as") == 0)
        return 0;

    const char *out = NULL;
    for (int i = 1; i + 1 < argc; ++i)
        if (strcmp(argv[i], "-o") == 0)
            out = argv[i + 1];
    if (out == NULL) {
        fprintf(stderr, "fakecc: no -o\n");
        return 1;
    }

    char buf[65536];
    while (read(0, buf, sizeof(buf)) > 0) {
    }

    int marker = open("/tmp/fakecc.running", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (marker >= 0)
        close(marker);

    /* Bounded wait: 120 s. */
    for (int i = 0; i < 2400 && access("/tmp/hold", F_OK) == 0; ++i)
        usleep(50 * 1000);

    int fd = open(out, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "fakecc: cannot create %s: %s\n", out, strerror(errno));
        return 1;
    }
    static const char obj[] = "FAKEOBJ\n";
    if (write(fd, obj, sizeof(obj) - 1) != (ssize_t)(sizeof(obj) - 1)) {
        fprintf(stderr, "fakecc: short write to %s\n", out);
        close(fd);
        return 1;
    }
    close(fd);
    unlink("/tmp/fakecc.running");
    return 0;
}

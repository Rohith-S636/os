#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

int main() {
    printf("Stress started...\n");

    while (1) {
        void *p = malloc(1024 * 1024); // 1 MB

        if (p == NULL) {
            printf("Memory allocation failed\n");
            break;
        }

        memset(p, 0, 1024 * 1024);

        printf("Allocating memory...\n");   // 🔥 IMPORTANT
        fflush(stdout);                    // 🔥 FORCE OUTPUT

        usleep(200000); // slow down (0.2 sec)
    }

    return 0;
}

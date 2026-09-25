#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "privacy.h"

int main(void) {
    const char *input = "JANE DOE; Jane, DOB 1980-01-02; jane.doe@example.com; +381 64 123 456; MRN: 998877\nBP stable.";
    char *output = anonymize(input, "Jane", "Doe", "1980-01-02");
    int passed = output && !contains_identity(output, "Jane", "Doe", "1980-01-02") &&
        !strstr(output, "998877") && strstr(output, "BP stable.");
    printf("anonymizer: %s\n", passed ? "ok" : "FAILED");
    free(output);
    const char *serbian = "Ime: Jelena Petrović\nDatum rođenja: 27.12.1958\nJMBG: 1234567890123\nKontrola uredna.";
    output = anonymize(serbian, "Jelena", "Petrović", "1958-12-27");
    passed = passed && output && !contains_identity(output, "Jelena", "Petrović", "1958-12-27") &&
        !strstr(output, "27.12.1958") && !strstr(output, "1234567890123") &&
        strstr(output, "Kontrola uredna.");
    printf("serbian anonymizer: %s\n", passed ? "ok" : "FAILED");
    free(output);
    return passed ? 0 : 1;
}

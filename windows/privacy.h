#ifndef CLINIC_PRIVACY_H
#define CLINIC_PRIVACY_H

/* UTF-8 input and heap-allocated UTF-8 output; NULL means do not send. */
char *anonymize(const char *text, const char *first, const char *last, const char *dob);
int contains_identity(const char *text, const char *first, const char *last, const char *dob);

#endif

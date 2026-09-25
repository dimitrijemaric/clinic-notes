#ifndef CLINIC_PRIVACY_H
#define CLINIC_PRIVACY_H

char *anonymize(const char *text, const char *first, const char *last, const char *dob);
int contains_identity(const char *text, const char *first, const char *last, const char *dob);

#endif

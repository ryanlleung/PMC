/* Text fit check, see textfit.c. Test builds only. */
#ifndef TEXTFIT_H
#define TEXTFIT_H
#include <stddef.h>
// Every label on the screen and the top layer that does not fit: one line
// per problem ("where | text | what"). Returns the count.
int textfit_check(char *buf, size_t n, const char *where);
// true: textfit_check lists every label with its text and box sizes.
extern bool textfit_all;
// The centre and size of every object a tap would reach: "x,y,w,h;..."
void textfit_clickables(char *buf, size_t n);
// The first text of the popup open (its title or question), "" if none.
void textfit_popup_text(char *buf, size_t n);
#endif

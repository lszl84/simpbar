#ifndef REGISTRY_H
#define REGISTRY_H

#include <stdbool.h>

struct bar;

void registry_init(struct bar *bar);
void registry_fini(struct bar *bar);

#endif /* REGISTRY_H */
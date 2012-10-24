#include <types.h>
#include <stddef.h>
#include "matrix/matrix.h"
#include "matrix/debug.h"
#include "notifier.h"

struct notifier_func {
	struct list link;	// Link to other functions
	void (*func)(void *);	// Function to call
	void *data;		// Data argument for function
};

void init_notifier(struct notifier *n)
{
	LIST_INIT(&n->functions);
}

void notifier_clear(struct notifier *n)
{
	;
}

void notifier_register(struct notifier *n, void (*func)(void *), void *data)
{
	struct notifier_func *nf;

	nf = kmalloc(sizeof(struct notifier), 0);
	LIST_INIT(&nf->link);
	nf->data = data;

	list_add_tail(&nf->link, &n->functions);
}

void notifier_unregister(struct notifier *n, void (*func)(void *), void *data)
{
	struct list *l;
	struct notifier_func *nf;

	LIST_FOR_EACH(l, &n->functions) {
		nf = LIST_ENTRY(l, struct notifier_func, link);
		if (nf->data == data) {
			list_del(&nf->link);
			kfree(nf);
			break;
		}
	}
}
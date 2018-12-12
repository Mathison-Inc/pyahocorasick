/*
	This is part of pyahocorasick Python module.
	
	AutomatonItemsIter implementation

    Author    : Wojciech Muła, wojciech_mula@poczta.onet.pl
    WWW       : http://0x80.pl
    License   : BSD-3-Clause (see LICENSE)
*/
#include "AutomatonItemsIter.h"

static PyTypeObject automaton_items_iter_type;


typedef struct AutomatonItemsStackItem {
	LISTITEM_data;

	struct TrieNode*	node;
	size_t depth;
} AutomatonItemsStackItem;

#define StackItem AutomatonItemsStackItem

static PyObject*
automaton_items_iter_new(
	Automaton* automaton,
	const TRIE_LETTER_TYPE* word,
	const ssize_t wordlen,

	const bool use_wildcard,
	const TRIE_LETTER_TYPE wildcard,
	const PatternMatchType matchtype
) {
	AutomatonItemsIter* iter;
	StackItem* new_item;

	iter = (AutomatonItemsIter*)F(PyObject_New)(AutomatonItemsIter, &automaton_items_iter_type);
	if (iter == NULL)
		return NULL;

	iter->automaton = automaton;
	iter->version	= automaton->version;
	iter->state	= NULL;
	iter->type = ITER_KEYS;
	iter->buffer = NULL;
#ifndef AHOCORASICK_UNICODE
	iter->char_buffer = NULL;
#endif
	iter->pattern = NULL;
	iter->use_wildcard = use_wildcard;
	iter->wildcard = wildcard;
	iter->matchtype = matchtype;
	list_init(&iter->stack);

	Py_INCREF((PyObject*)iter->automaton);

	iter->buffer = memory_alloc((automaton->longest_word + 1) * TRIE_LETTER_SIZE);
	if (iter->buffer == NULL) {
		goto no_memory;
	}

#ifndef AHOCORASICK_UNICODE
	iter->char_buffer = memory_alloc(automaton->longest_word + 1);
	if (iter->char_buffer == NULL) {
		goto no_memory;
	}
#endif

	if (word) {
		iter->pattern = (TRIE_LETTER_TYPE*)memory_alloc(wordlen * TRIE_LETTER_SIZE);
		if (UNLIKELY(iter->pattern == NULL)) {
			goto no_memory;
		}
		else {
			iter->pattern_length = wordlen;
			memcpy(iter->pattern, word, wordlen * TRIE_LETTER_SIZE);
		}
	}
	else
		iter->pattern_length = 0;
	
	new_item = (StackItem*)list_item_new(sizeof(StackItem));
	if (UNLIKELY(new_item == NULL)) {
		goto no_memory;
	}

	new_item->node = automaton->root;
	new_item->depth = 0;
	list_push_front(&iter->stack, (ListItem*)new_item);

	return (PyObject*)iter;

no_memory:
	Py_DECREF((PyObject*)iter);
	PyErr_NoMemory();
	return NULL;
}


#define iter ((AutomatonItemsIter*)self)

static void
automaton_items_iter_del(PyObject* self) {
	xfree(iter->buffer);
	xfree(iter->pattern);
#ifndef AHOCORASICK_UNICODE
	xfree(iter->char_buffer);
#endif

	list_delete(&iter->stack);
	Py_DECREF(iter->automaton);

	PyObject_Del(self);
}


static PyObject*
automaton_items_iter_iter(PyObject* self) {
	Py_INCREF(self);
	return self;
}


static PyObject*
automaton_items_iter_next(PyObject* self) {

	bool output;
    TrieNode* node;
    size_t depth;

	if (UNLIKELY(iter->version != iter->automaton->version)) {
		PyErr_SetString(PyExc_ValueError, "The underlying automaton has changed: this iterator is no longer valid.");
		return NULL;
	}

	while (true) {
		StackItem* top = (StackItem*)list_pop_first(&iter->stack);
		if (top == NULL)
			return NULL; /* Stop iteration */

        if (top->node == NULL) {
            memory_free(top);
            return NULL; /* Stop iteration */
        }

        node  = top->node;
        depth = top->depth;
        memory_free(top);

		if (iter->matchtype != MATCH_AT_LEAST_PREFIX and depth > iter->pattern_length)
			continue;

		switch (iter->matchtype) {
			case MATCH_EXACT_LENGTH:
				output = (depth == iter->pattern_length);
				break;

			case MATCH_AT_MOST_PREFIX:
				output = (depth <= iter->pattern_length);
				break;
				
			case MATCH_AT_LEAST_PREFIX:
			default:
				output = (depth >= iter->pattern_length);
				break;

		}

		iter->state = node;
		if ((depth >= iter->pattern_length) or
		    (iter->use_wildcard and iter->pattern[depth] == iter->wildcard)) {

			// process all
			const int n = iter->state->n;
			int i;
			for (i=0; i < n; i++) {
				StackItem* new_item = (StackItem*)list_item_new(sizeof(StackItem));
				if (UNLIKELY(new_item == NULL)) {
					PyErr_NoMemory();
					return NULL;
				}

				new_item->node  = iter->state->next[i];
				new_item->depth = depth + 1;
				list_push_front(&iter->stack, (ListItem*)new_item);
			}
		}
		else {
			// process single letter
			TrieNode* node = trienode_get_next(iter->state, iter->pattern[depth]);

			if (node) {
				StackItem* new_item = (StackItem*)list_item_new(sizeof(StackItem));
				if (UNLIKELY(new_item == NULL)) {
					PyErr_NoMemory();
					return NULL;
				}

				new_item->node  = node;
				new_item->depth = depth + 1;
				list_push_front(&iter->stack, (ListItem*)new_item);
			}
		}

		if (iter->type != ITER_VALUES)
			// update keys when needed
			iter->buffer[depth] = iter->state->letter;
#ifndef AHOCORASICK_UNICODE
			iter->char_buffer[depth] = (char)iter->state->letter;
#endif
		if (output and iter->state->eow) {
			PyObject* val;

			switch (iter->type) {
				case ITER_KEYS:
#if defined PEP393_UNICODE
					return F(PyUnicode_FromKindAndData)(PyUnicode_4BYTE_KIND, (void*)(iter->buffer + 1), depth);
#elif defined AHOCORASICK_UNICODE
					return PyUnicode_FromUnicode((Py_UNICODE*)(iter->buffer + 1), depth);
#else
					return PyBytes_FromStringAndSize(iter->char_buffer + 1, depth);
#endif

				case ITER_VALUES:
                    val = iter->state->output.object;
                    Py_INCREF(val);
					return val;

				case ITER_ITEMS:
					return F(Py_BuildValue)(
#ifdef PY3K
    #ifdef AHOCORASICK_UNICODE
							"(u#O)", /*key*/ iter->buffer + 1, depth,
    #else
						    "(y#O)", /*key*/ iter->buffer + 1, depth,
    #endif
#else
                            "(s#O)", /*key*/ iter->char_buffer + 1, depth,
#endif
							/*val*/ iter->state->output.object
					);
			} // switch
		}
	}
}

#undef StackItem
#undef iter

static PyTypeObject automaton_items_iter_type = {
	PY_OBJECT_HEAD_INIT
	"AutomatonItemsIter",						/* tp_name */
	sizeof(AutomatonItemsIter),					/* tp_size */
	0,											/* tp_itemsize? */
	(destructor)automaton_items_iter_del,		/* tp_dealloc */
	0,                                      	/* tp_print */
	0,                                         	/* tp_getattr */
	0,                                          /* tp_setattr */
	0,                                          /* tp_reserved */
	0,											/* tp_repr */
	0,                                          /* tp_as_number */
	0,                                          /* tp_as_sequence */
	0,                                          /* tp_as_mapping */
	0,                                          /* tp_hash */
	0,                                          /* tp_call */
	0,                                          /* tp_str */
	PyObject_GenericGetAttr,                    /* tp_getattro */
	0,                                          /* tp_setattro */
	0,                                          /* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,                         /* tp_flags */
	0,                                          /* tp_doc */
	0,                                          /* tp_traverse */
	0,                                          /* tp_clear */
	0,                                          /* tp_richcompare */
	0,                                          /* tp_weaklistoffset */
	automaton_items_iter_iter,					/* tp_iter */
	automaton_items_iter_next,					/* tp_iternext */
	0,											/* tp_methods */
	0,						                	/* tp_members */
	0,                                          /* tp_getset */
	0,                                          /* tp_base */
	0,                                          /* tp_dict */
	0,                                          /* tp_descr_get */
	0,                                          /* tp_descr_set */
	0,                                          /* tp_dictoffset */
	0,                                          /* tp_init */
	0,                                          /* tp_alloc */
	0,                                          /* tp_new */
};

#include "span.h"

#include <SAPI.h>
#include "priority_sampling/priority_sampling.h"
#include <time.h>
#include <unistd.h>

#include "auto_flush.h"
#include "compat_string.h"
#include "configuration.h"
#include "ddtrace.h"
#include "logging.h"
#include "random.h"
#include "serializer.h"

#define USE_REALTIME_CLOCK 0
#define USE_MONOTONIC_CLOCK 1

ZEND_EXTERN_MODULE_GLOBALS(ddtrace);

void ddtrace_init_span_stacks(void) {
    DDTRACE_G(active_stack) = NULL;
    DDTRACE_G(active_stack) = ddtrace_init_root_span_stack();
    DDTRACE_G(top_closed_stack) = NULL;
    DDTRACE_G(open_spans_count) = 0;
    DDTRACE_G(dropped_spans_count) = 0;
    DDTRACE_G(closed_spans_count) = 0;
}

static void dd_drop_span_nodestroy(ddtrace_span_data *span, bool silent) {
    span->duration = silent ? DDTRACE_SILENTLY_DROPPED_SPAN : DDTRACE_DROPPED_SPAN;
}

static void dd_drop_span(ddtrace_span_data *span, bool silent) {
    dd_drop_span_nodestroy(span, silent);
    OBJ_RELEASE(&span->std);
}

static void dd_free_span_ring(ddtrace_span_data *span) {
    if (span != NULL) {
        ddtrace_span_data *cur = span;
        do {
            ddtrace_span_data *tmp = cur;
            cur = cur->next;
#if PHP_VERSION_ID < 70400
            // remove the artificially increased RC while closing again
            GC_DELREF(&tmp->std);
#endif
            OBJ_RELEASE(&tmp->std);
        } while (cur != span);
    }
}

void ddtrace_free_span_stacks(bool silent) {
    zend_objects_store *objects = &EG(objects_store);
    zend_object **end = objects->object_buckets + 1;
    zend_object **obj_ptr = objects->object_buckets + objects->top;

    OBJ_RELEASE(&DDTRACE_G(active_stack)->std);

    do {
        obj_ptr--;
        zend_object *obj = *obj_ptr;
        if (IS_OBJ_VALID(obj) && obj->ce == ddtrace_ce_span_stack) {
            ddtrace_span_stack *stack = (ddtrace_span_stack *)obj;

            // temporarily addref to avoid freeing the stack during it being processed
            GC_ADDREF(&stack->std);

            if (stack->active && stack->active->stack == stack) {
                ddtrace_span_data *active_span = stack->active;
                stack->root_span = NULL;

                ddtrace_span_data *span = active_span->parent;
                while (span && span->stack == stack) {
                    dd_drop_span_nodestroy(span, silent);
                    span = span->parent;
                }

                stack->active = span;
                if (span) {
                    GC_ADDREF(&span->std);
                } else {
                    ZVAL_NULL(&stack->property_active);
                }

                // drop the active span last, it holds the start of the span "chain" of parents which each hold a ref to the next
                dd_drop_span(active_span, silent);
            }

            dd_free_span_ring(stack->closed_ring);
            stack->closed_ring = NULL;

            // We hold a ref if it's waiting for being flushed
            if (stack->closed_ring_flush != NULL) {
                GC_DELREF(&stack->std);
            }
            dd_free_span_ring(stack->closed_ring_flush);
            stack->closed_ring_flush = NULL;

            OBJ_RELEASE(&stack->std);
        }
    } while (obj_ptr != end);

    DDTRACE_G(open_spans_count) = 0;
    DDTRACE_G(dropped_spans_count) = 0;
    DDTRACE_G(closed_spans_count) = 0;
    DDTRACE_G(active_stack) = NULL;
    DDTRACE_G(top_closed_stack) = NULL;
}

static uint64_t _get_nanoseconds(bool monotonic_clock) {
    struct timespec time;
    if (clock_gettime(monotonic_clock ? CLOCK_MONOTONIC : CLOCK_REALTIME, &time) == 0) {
        return time.tv_sec * 1000000000L + time.tv_nsec;
    }
    return 0;
}

void ddtrace_open_span(ddtrace_span_data *span) {
    ddtrace_span_stack *stack = DDTRACE_G(active_stack);
    bool primary_stack = stack->parent_stack == NULL;

    if (primary_stack) {
        stack = ddtrace_init_root_span_stack();
        ddtrace_switch_span_stack(stack);
        // We don't hold a direct reference to the active stack
        GC_DELREF(&stack->std);
    }

    // ensure dtor can be called again
    GC_DEL_FLAGS(&stack->std, IS_OBJ_DESTRUCTOR_CALLED);

    // All open spans hold a ref to their stack
    ZVAL_OBJ_COPY(&span->property_stack, &stack->std);

    span->span_id = ddtrace_generate_span_id();
    // if not a root span or the true root span (distributed tracing)
    bool root_span = DDTRACE_G(active_stack)->root_span == NULL;
    if (!root_span || primary_stack) {
        // Inherit from our current parent
        span->parent_id = ddtrace_peek_span_id();
        span->trace_id = ddtrace_peek_trace_id();
        if (span->trace_id == 0) {
            span->trace_id = span->span_id;
        }
    } else {
        // custom new traces
        span->parent_id = 0;
        span->trace_id = span->span_id;
    }
    span->duration_start = _get_nanoseconds(USE_MONOTONIC_CLOCK);
    // Start time is nanoseconds from unix epoch
    // @see https://docs.datadoghq.com/api/?lang=python#send-traces
    span->start = _get_nanoseconds(USE_REALTIME_CLOCK);

    ddtrace_span_data *parent_span = DDTRACE_G(active_stack)->active;
    ZVAL_OBJ(&DDTRACE_G(active_stack)->property_active, &span->std);
    ++DDTRACE_G(open_spans_count);

    // It just became the active span, so incref it.
    GC_ADDREF(&span->std);

    if (root_span) {
        DDTRACE_G(active_stack)->root_span = span;

        ZVAL_NULL(&span->property_parent);
        span->parent = NULL;

        ddtrace_set_root_span_properties(span);
    } else {
        // do not copy the parent, it was active span before, just transfer that reference
        ZVAL_OBJ(&span->property_parent, &parent_span->std);
        ZVAL_COPY(ddtrace_spandata_property_service(span), ddtrace_spandata_property_service(parent_span));
        ZVAL_COPY(ddtrace_spandata_property_type(span), ddtrace_spandata_property_type(parent_span));
    }

    span->root = DDTRACE_G(active_stack)->root_span;

    ddtrace_set_global_span_properties(span);
}

ddtrace_span_data *ddtrace_alloc_execute_data_span(zend_ulong index, zend_execute_data *execute_data) {
    zval *span_zv = zend_hash_index_find(&DDTRACE_G(traced_spans), index);
    ddtrace_span_data *span;
    if (span_zv) {
        span = Z_PTR_P(span_zv);
        ++Z_TYPE_INFO_P(span_zv);
    } else {
        span = ddtrace_init_span(DDTRACE_INTERNAL_SPAN);
        ddtrace_open_span(span);

        // SpanData::$name defaults to fully qualified called name
        zval *prop_name = ddtrace_spandata_property_name(span);

        if (EX(func) && EX(func)->common.function_name) {
            zval_ptr_dtor(prop_name);

            zend_class_entry *called_scope = EX(func)->common.scope ? zend_get_called_scope(execute_data) : NULL;
            if (called_scope) {
                // This cannot be cached on the dispatch since subclasses can share the same parent dispatch
                ZVAL_STR(prop_name, strpprintf(0, "%s.%s", ZSTR_VAL(called_scope->name), ZSTR_VAL(EX(func)->common.function_name)));
            } else {
                ZVAL_STR_COPY(prop_name, EX(func)->common.function_name);
            }
        }

        zval zv;
        Z_PTR(zv) = span;
        Z_TYPE_INFO(zv) = 2;
        zend_hash_index_add_new(&DDTRACE_G(traced_spans), index, &zv);
    }
    return span;
}

void ddtrace_clear_execute_data_span(zend_ulong index, bool keep) {
    zval *span_zv = zend_hash_index_find(&DDTRACE_G(traced_spans), index);
    ddtrace_span_data *span = Z_PTR_P(span_zv);
    if (--Z_TYPE_INFO_P(span_zv) == 1 || !keep) {
        if (!ddtrace_span_is_dropped(span)) {
            if (keep) {
                ddtrace_close_span(span);
            } else {
                ddtrace_drop_span(span);
                span->duration = DDTRACE_SILENTLY_DROPPED_SPAN;
            }
        }
    }
    if (Z_TYPE_INFO_P(span_zv) == 1) {
        OBJ_RELEASE(&span->std);
        zend_hash_index_del(&DDTRACE_G(traced_spans), index);
    }
}

void ddtrace_switch_span_stack(ddtrace_span_stack *target_stack) {
    GC_ADDREF(&target_stack->std);
    OBJ_RELEASE(&DDTRACE_G(active_stack)->std);
    DDTRACE_G(active_stack) = target_stack;
}

ddtrace_span_data *ddtrace_init_span(enum ddtrace_span_dataype type) {
    zval fci_zv;
    object_init_ex(&fci_zv, ddtrace_ce_span_data);
    ddtrace_span_data *span = (ddtrace_span_data *)Z_OBJ(fci_zv);
    span->type = type;
    return span;
}

static ddtrace_span_stack *dd_alloc_span_stack(void) {
    zval fci_zv;
    object_init_ex(&fci_zv, ddtrace_ce_span_stack);
    ddtrace_span_stack *span_stack = (ddtrace_span_stack *)Z_OBJ(fci_zv);
    return span_stack;
}

ddtrace_span_stack *ddtrace_init_root_span_stack(void) {
    ddtrace_span_stack *span_stack = dd_alloc_span_stack();
    if (DDTRACE_G(active_stack)) {
        ZVAL_OBJ_COPY(&span_stack->property_parent, &DDTRACE_G(active_stack)->std);
    } else {
        ZVAL_NULL(&span_stack->property_parent);
        span_stack->parent_stack = NULL;
    }
    ZVAL_NULL(&span_stack->property_active);
    span_stack->root_stack = span_stack;
    span_stack->root_span = NULL;
    return span_stack;
}

ddtrace_span_stack *ddtrace_init_span_stack(void) {
    ddtrace_span_stack *span_stack = dd_alloc_span_stack();
    ZVAL_OBJ_COPY(&span_stack->property_parent, &DDTRACE_G(active_stack)->std);
    ZVAL_COPY(&span_stack->property_active, &DDTRACE_G(active_stack)->property_active);
    span_stack->root_stack = DDTRACE_G(active_stack)->root_stack;
    span_stack->root_span = DDTRACE_G(active_stack)->root_span;
    return span_stack;
}

void ddtrace_push_root_span(void) {
    ddtrace_span_data *span = ddtrace_init_span(DDTRACE_AUTOROOT_SPAN);
    ddtrace_open_span(span);
    // We opened the span, but are not going to hold a reference to it directly - the stack will manage it.
    GC_DELREF(&span->std);
}

DDTRACE_PUBLIC bool ddtrace_root_span_add_tag(zend_string *tag, zval *value) {
    if (!DDTRACE_G(active_stack)) {
        return false;
    }

    ddtrace_span_data *root_span = DDTRACE_G(active_stack)->root_span;
    if (root_span == NULL) {
        return false;
    }

    return zend_hash_add(ddtrace_spandata_property_meta(root_span), tag, value) != NULL;
}

bool ddtrace_span_alter_root_span_config(zval *old_value, zval *new_value) {
    if (Z_TYPE_P(old_value) == Z_TYPE_P(new_value) || DDTRACE_G(disable)) {
        return true;
    }

    if (Z_TYPE_P(old_value) == IS_FALSE) {
        if (DDTRACE_G(active_stack) == NULL) {
            ddtrace_push_root_span();
            return true;
        }
        return false;
    } else {
        if (DDTRACE_G(active_stack)->root_span == NULL) {
            return true;  // might be the case after serialization
        }
        if (DDTRACE_G(active_stack)->active == DDTRACE_G(active_stack)->root_span && DDTRACE_G(active_stack)->closed_ring == NULL) {
            ddtrace_span_data *span = DDTRACE_G(active_stack)->root_span;
            DDTRACE_G(active_stack)->root_span = NULL; // As a special case, always hard-drop a root span dropped due to a config change
            ddtrace_drop_span(span);
            return true;
        } else {
            return false;
        }
    }
}

void dd_trace_stop_span_time(ddtrace_span_data *span) {
    span->duration = _get_nanoseconds(USE_MONOTONIC_CLOCK) - span->duration_start;
}

bool ddtrace_has_top_internal_span(ddtrace_span_data *end) {
    ddtrace_span_data *span = end->stack->active;
    while (span) {
        if (span == end) {
            return true;
        }
        if (span->type != DDTRACE_USER_SPAN) {
            return false;
        }
        span = span->parent;
    }
    return false;
}

void ddtrace_close_userland_spans_until(ddtrace_span_data *until) {
    ddtrace_span_data *span;
    while ((span = until->stack->active) && span->stack == until->stack && span != until && span->type != DDTRACE_AUTOROOT_SPAN) {
        if (span->type == DDTRACE_INTERNAL_SPAN) {
            ddtrace_log_err("Found internal span data while closing userland spans");
        }

        zend_string *name = ddtrace_convert_to_str(ddtrace_spandata_property_name(span));
        ddtrace_log_debugf("Found unfinished span while automatically closing spans with name '%s'", ZSTR_VAL(name));
        zend_string_release(name);

        if (get_DD_AUTOFINISH_SPANS()) {
            dd_trace_stop_span_time(span);
            ddtrace_close_span(span);
        } else {
            ddtrace_drop_span(span);
        }
    }
}

static void dd_mark_closed_spans_flushable(ddtrace_span_stack *stack) {
    if (stack->closed_ring) {
        // Note: here is the reason why the closed spans need to be a ring instead of having a NULL sentinel in ->next:
        // It's impossible to fuse two simple linked lists without walking one to its end and updating its last ->next pointer.
        // Having a circular structure just allows to splice the one into the other at an arbitrary location
        if (stack->closed_ring_flush) {
            stack->closed_ring->next = stack->closed_ring_flush->next;
            stack->closed_ring_flush->next = stack->closed_ring;
        } else {
            stack->closed_ring_flush = stack->closed_ring;

            // As long as there's something to flush, we must hold a reference (to avoid cycle collection)
            GC_ADDREF(&stack->std);

            if (stack->root_span->stack == stack || stack->root_span->type == DDTRACE_SPAN_CLOSED) {
                stack->next = DDTRACE_G(top_closed_stack);
                DDTRACE_G(top_closed_stack) = stack;
            } else {
                // we'll just attach it so that it'll be flushed together (i.e. chunks are not flushed _before_ the root stack)
                stack->next = stack->root_stack->top_closed_stack;
                stack->root_stack->top_closed_stack = stack;
            }
        }
        stack->closed_ring = NULL;
    }
}

// closing a chunks last span:
// check if any parent has open spans
// if not, autoflush / add to closed stacks chain
// if yes, do nothing
static void dd_close_entry_span_of_stack(ddtrace_span_stack *stack) {
    // We need to track complete finished span stacks separately in order to mark them flushable
    dd_mark_closed_spans_flushable(stack);

    if (!stack->root_span || stack->root_span->stack == stack) {
        // Enforce a sampling decision here
        ddtrace_fetch_prioritySampling_from_root();

        // Root span stacks are automatic and tied to the lifetime of that root
        stack->root_span = NULL;
        if (stack == stack->root_stack && DDTRACE_G(active_stack) == stack) {
            // We are always active stack except if ddtrace_close_top_span_without_stack_swap is used
            ddtrace_switch_span_stack(stack->parent_stack);
        }

        if (get_DD_TRACE_AUTO_FLUSH_ENABLED() && ddtrace_flush_tracer() == FAILURE) {
            // In case we have root spans enabled, we need to always flush if we close that one (RSHUTDOWN)
            ddtrace_log_debug("Unable to auto flush the tracer");
        }
    }
}

void ddtrace_close_span(ddtrace_span_data *span) {
    if (span == NULL || !ddtrace_has_top_internal_span(span) || span->type == DDTRACE_SPAN_CLOSED) {
        return;
    }

    // Closing a span (esp. when leaving a traced function) autoswitches the stacks if necessary
    if (span->stack != DDTRACE_G(active_stack)) {
        ddtrace_switch_span_stack(span->stack);
    }

    ddtrace_close_userland_spans_until(span);

    ddtrace_close_top_span_without_stack_swap(span);
}

void ddtrace_close_top_span_without_stack_swap(ddtrace_span_data *span) {
    ddtrace_span_stack *stack = span->stack;

    span->type = DDTRACE_SPAN_CLOSED;

    stack->active = span->parent;
    // The top span is always referenced by the span stack
    if (stack->active) {
        GC_ADDREF(&stack->active->std);
    } else {
        ZVAL_NULL(&stack->property_active);
    }
#if PHP_VERSION_ID < 70400
    // On PHP 7.3 and prior PHP will just destroy all unchanged references in cycle collection, in particular given that it does not appear in get_gc
    // Artificially increase refcount here thus.
    GC_ADDREF(&span->std);
#endif

    ++DDTRACE_G(closed_spans_count);
    --DDTRACE_G(open_spans_count);

    // Move the reference ("top span") to the closed list
    if (stack->closed_ring) {
        span->next = stack->closed_ring->next;
        stack->closed_ring->next = span;
    } else {
        span->next = span;
        stack->closed_ring = span;
    }

    if (!stack->active || stack->active->stack != stack) {
        dd_close_entry_span_of_stack(stack);
    }
}

// i.e. what DDTrace\active_span() reports. DDTrace\active_stack()->active is the active span which will be used as parent for new spans on that stack
ddtrace_span_data *ddtrace_active_span(void) {
    ddtrace_span_stack *stack = DDTRACE_G(active_stack);
    if (!stack) {
        return NULL;
    }

    ddtrace_span_stack *end = stack->root_stack->parent_stack;

    do {
        if (stack->active && stack->active->stack == stack) {
            return stack->active;
        }
        stack = stack->parent_stack;
    } while (stack != end);

    return NULL;
}

void ddtrace_close_all_open_spans(bool force_close_root_span) {
    zend_objects_store *objects = &EG(objects_store);
    zend_object **end = objects->object_buckets + 1;
    zend_object **obj_ptr = objects->object_buckets + objects->top;

    do {
        obj_ptr--;
        zend_object *obj = *obj_ptr;
        if (IS_OBJ_VALID(obj) && obj->ce == ddtrace_ce_span_stack) {
            ddtrace_span_stack *stack = (ddtrace_span_stack *)obj;

            // temporarily addref to avoid freeing the stack during it being processed
            GC_ADDREF(&stack->std);

            ddtrace_span_data *span;
            while ((span = stack->active) && span->stack == stack) {
                if (get_DD_AUTOFINISH_SPANS() || (force_close_root_span && span->type == DDTRACE_AUTOROOT_SPAN)) {
                    dd_trace_stop_span_time(span);
                    ddtrace_close_span(span);
                } else {
                    ddtrace_drop_span(span);
                }
            }

            OBJ_RELEASE(&stack->std);
        }
    } while (obj_ptr != end);
}

void ddtrace_mark_all_span_stacks_flushable(void) {
    zend_objects_store *objects = &EG(objects_store);
    zend_object **end = objects->object_buckets + 1;
    zend_object **obj_ptr = objects->object_buckets + objects->top;

    do {
        obj_ptr--;
        zend_object *obj = *obj_ptr;
        if (IS_OBJ_VALID(obj) && obj->ce == ddtrace_ce_span_stack) {
            dd_mark_closed_spans_flushable((ddtrace_span_stack *)obj);
        }
    } while (obj_ptr != end);
}

void ddtrace_drop_span(ddtrace_span_data *span) {
    ddtrace_span_stack *stack = span->stack;

    // Closing/Dropping a span (esp. when leaving a traced function) autoswitches the stacks if necessary
    if (stack != DDTRACE_G(active_stack)) {
        ddtrace_switch_span_stack(span->stack);
    }

    // As a special case dropping a root span rejects it to avoid traces without root span
    if (stack->root_span == span) {
        ddtrace_set_prioritySampling_on_root(PRIORITY_SAMPLING_USER_REJECT);
        dd_trace_stop_span_time(span);
        ddtrace_close_span(span);
        return;
    }

    stack->active = span->parent;
    // The top span is always referenced by the span stack
    if (stack->active) {
        GC_ADDREF(&stack->active->std);
    } else {
        ZVAL_NULL(&stack->property_active);
    }

    ++DDTRACE_G(dropped_spans_count);
    --DDTRACE_G(open_spans_count);

    if (!stack->active || stack->active->stack != stack) {
        dd_close_entry_span_of_stack(stack);
    }

    dd_drop_span(span, false);
}

void ddtrace_serialize_closed_spans(zval *serialized) {
    array_init(serialized);

    // We need to loop here, as closing the last span root stack could add other spans here
    while (DDTRACE_G(top_closed_stack)) {
        ddtrace_span_stack *rootstack = DDTRACE_G(top_closed_stack);
        DDTRACE_G(top_closed_stack) = NULL;
        do {
            ddtrace_span_stack *stack = rootstack;
            rootstack = rootstack->next;
            ddtrace_span_stack *next_stack = stack->top_closed_stack;
            stack->top_closed_stack = NULL;
            do {
                // Note this ->next: We always splice in new spans at next, so start at next to mostly preserve order
                ddtrace_span_data *span = stack->closed_ring_flush->next, *end = span;
                stack->closed_ring_flush = NULL;
                do {
                    ddtrace_span_data *tmp = span;
                    span = tmp->next;
                    ddtrace_serialize_span_to_array(tmp, serialized);
#if PHP_VERSION_ID < 70400
                    // remove the artificially increased RC while closing again
                    GC_DELREF(&tmp->std);
#endif
                    OBJ_RELEASE(&tmp->std);
                } while (span != end);
                // We hold a reference to stacks with flushable spans
                OBJ_RELEASE(&stack->std);
                // Note: if a stack gets a fresh closed_ring_flush (e.g. due to gc during serialization), the root span will have been closed by now.
                // Thus it's appended to top_closed_stack and we do not need to recheck closed_ring_flush here.

                stack = next_stack;
                if (stack) {
                    next_stack = stack->next;
                }
            } while (stack);
        } while (rootstack);

        // Also flush possible cycles here
        zend_gc_collect_cycles();
    }
}

zend_string *ddtrace_span_id_as_string(uint64_t id) { return zend_strpprintf(0, "%" PRIu64, id); }

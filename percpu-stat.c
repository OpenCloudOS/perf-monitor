#include <stdio.h>
#include <stdlib.h>
#include <linux/rblist.h>
#include "monitor.h"
#include "trace_helpers.h"
#include "tep.h"

static profiler percpu_stat;

struct swevent_stat {
    uint64_t count;
    uint64_t diff;
};
struct evsel_node {
    struct rb_node rbnode;
    struct evsel_node *next;
    struct perf_evsel *evsel;
    const char *name;
    int name_len;
    bool cpu_idle;
    struct swevent_stat *stats;
};

struct monitor_ctx {
    int nr_ins;
    struct evsel_node *first;
    struct evsel_node **p_next;
    struct rblist evsel_list;
    struct env *env;
} ctx;

static int evsel_node_cmp(struct rb_node *rbn, const void *entry)
{
    struct evsel_node *e = container_of(rbn, struct evsel_node, rbnode);
    const struct evsel_node *n = entry;

    if (e->evsel > n->evsel)
        return 1;
    else if (e->evsel < n->evsel)
        return -1;
    else
        return 0;
}

static struct rb_node *evsel_node_new(struct rblist *rlist, const void *new_entry)
{
    const struct evsel_node *n = new_entry;
    struct evsel_node *e = malloc(sizeof(*e));
    if (e) {
        e->next = NULL;
        e->evsel = n->evsel;
        e->name = n->name;
        e->name_len = (int)strlen(e->name);
        e->cpu_idle = n->cpu_idle;
        e->stats = calloc(ctx.nr_ins, sizeof(*e->stats));
        if (!e->stats) {
            free(e);
            return NULL;
        }
        *ctx.p_next = e;
        ctx.p_next = &e->next;
        RB_CLEAR_NODE(&e->rbnode);
        return &e->rbnode;
    } else
        return NULL;
}

static void evsel_node_delete(struct rblist *rblist, struct rb_node *rb_node)
{
    struct evsel_node *e = container_of(rb_node, struct evsel_node, rbnode);
    free(e->stats);
    free(e);
}

static int monitor_ctx_init(struct env *env)
{
    ctx.nr_ins = monitor_nr_instance();

    ctx.first = NULL;
    ctx.p_next = &ctx.first;

    rblist__init(&ctx.evsel_list);
    ctx.evsel_list.node_cmp = evsel_node_cmp;
    ctx.evsel_list.node_new = evsel_node_new;
    ctx.evsel_list.node_delete = evsel_node_delete;

    tep__ref();
    return 0;
}

static void monitor_ctx_exit(void)
{
    tep__unref();
    rblist__exit(&ctx.evsel_list);
}

static struct perf_evsel *perf_tp_event(struct perf_evlist *evlist, const char *sys, const char *name)
{
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 0,
        .freq          = 0,
        .sample_type   = 0,
        .read_format   = 0,
        .pinned        = 1,
        .disabled      = 1,
    };
    struct perf_evsel *evsel;
    int id;

    id = tep__event_id(sys, name);
    if (id < 0)
        return NULL;

    attr.config = id;
    evsel = perf_evsel__new(&attr);
    if (!evsel) {
        return NULL;
    }
    perf_evlist__add(evlist, evsel);

    return evsel;
}

static struct perf_evsel *perf_sw_event(struct perf_evlist *evlist, int config)
{
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_SOFTWARE,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 0,
        .freq          = 0,
        .sample_type   = 0,
        .read_format   = 0,
        .pinned        = 1,
        .disabled      = 1,
    };
    struct perf_evsel *evsel;

    attr.config = config;
    evsel = perf_evsel__new(&attr);
    if (!evsel) {
        return NULL;
    }
    perf_evlist__add(evlist, evsel);

    return evsel;
}

static void __evsel_name(struct perf_evsel *evsel, const char *name, bool cpu_idle)
{
    struct evsel_node n;
    if (evsel) {
        n.evsel = evsel;
        n.name = name;
        n.cpu_idle = cpu_idle;
        rblist__add_node(&ctx.evsel_list, &n);
    }
}
#define evsel_name(evsel, name) __evsel_name((evsel), (name), false)
static int percpu_stat_init(struct perf_evlist *evlist, struct env *env)
{
    if (monitor_ctx_init(env) < 0)
        return -1;

    if (env->interval == 0)
        env->interval = 1000;

    //software event
    evsel_name(perf_sw_event(evlist, PERF_COUNT_SW_CONTEXT_SWITCHES), " SOFT csw");
    evsel_name(perf_sw_event(evlist, PERF_COUNT_SW_CPU_MIGRATIONS), "cpu-mig");
    evsel_name(perf_sw_event(evlist, PERF_COUNT_SW_PAGE_FAULTS_MIN), "minflt");
    evsel_name(perf_sw_event(evlist, PERF_COUNT_SW_PAGE_FAULTS_MAJ), "majflt");
    //syscalls
    if (env->syscalls)
        evsel_name(perf_tp_event(evlist, "raw_syscalls", "sys_enter"), " syscalls");
    //irq, softirq, workqueue
    evsel_name(perf_tp_event(evlist, "irq", "irq_handler_entry"), " hardirq");
    evsel_name(perf_tp_event(evlist, "irq", "softirq_entry"), "softirq");
    evsel_name(perf_tp_event(evlist, "timer", "hrtimer_expire_entry"), "hrtimer");
    evsel_name(perf_tp_event(evlist, "workqueue", "workqueue_execute_start"), "workqueue");
    //kvm
    evsel_name(perf_tp_event(evlist, "kvm", "kvm_exit"), " KVM exit");
    //net
    evsel_name(perf_tp_event(evlist, "net", "netif_receive_skb"), " NET recv");
    evsel_name(perf_tp_event(evlist, "net", "net_dev_xmit"), " xmit");
    evsel_name(perf_tp_event(evlist, "net", "napi_gro_receive_entry"), "gro");
    //page alloc
    evsel_name(perf_tp_event(evlist, "kmem", "mm_page_alloc"), " MM alloc");
    evsel_name(perf_tp_event(evlist, "compaction", "mm_compaction_migratepages"), "compact");
    evsel_name(perf_tp_event(evlist, "vmscan", "mm_vmscan_direct_reclaim_begin"), "reclaim");
    evsel_name(perf_tp_event(evlist, "migrate", "mm_migrate_pages"), "migrate");
    //page cache
    evsel_name(perf_tp_event(evlist, "filemap", "mm_filemap_add_to_page_cache"), " PAGE cache");
    evsel_name(perf_tp_event(evlist, "writeback", "wbc_writepage"), " WB pages");
    //cpu_idle
    __evsel_name(perf_tp_event(evlist, "power", "cpu_idle"), " idle", true);

    return 0;
}

static void percpu_stat_exit(struct perf_evlist *evlist)
{
    monitor_ctx_exit();
}

static void percpu_stat_read(struct perf_evsel *evsel, struct perf_counts_values *count, int instance)
{
    struct evsel_node n = {.evsel = evsel};
    struct rb_node *rbn = rblist__find(&ctx.evsel_list, &n);
    struct evsel_node *e = rbn ? container_of(rbn, struct evsel_node, rbnode) : NULL;

    if (e == NULL)
        return;

    e->stats[instance].diff = 0;
    if (count->val > e->stats[instance].count) {
        e->stats[instance].diff = count->val - e->stats[instance].count;
        e->stats[instance].count = count->val;
        if (e->cpu_idle) {
            //cpu_idle, contains enter and exit, must be divided by 2
            e->stats[instance].diff /= 2;
        }
    }
}

static void percpu_stat_interval(void)
{
    struct evsel_node *next = ctx.first;
    int ins;

    print_time(stdout);
    printf("\n[CPU] ");
    while (next) {
        printf("%s ", next->name);
        next = next->next;
    }

    for (ins = 0; ins < ctx.nr_ins; ins ++) {
        printf("\n[%03d] ", monitor_instance_cpu(ins));
        next = ctx.first;
        while (next) {
            printf("%*lu ", next->name_len, next->stats[ins].diff);
            next = next->next;
        }
    }
    printf("\n");
}

static void percpu_stat_sample(union perf_event *event, int instance)
{
}

static profiler percpu_stat = {
    .name = "percpu-stat",
    .pages = 0,
    .init = percpu_stat_init,
    .deinit = percpu_stat_exit,
    .interval = percpu_stat_interval,
    .read   = percpu_stat_read,
    .sample = percpu_stat_sample,
};
PROFILER_REGISTER(percpu_stat)


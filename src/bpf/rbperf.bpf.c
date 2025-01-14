// Copyright (c) Facebook, Inc. and its affiliates.
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// Copyright (c) 2022 The rbperf authors

#include "rbperf.h"

#include "vmlinux.h"

#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

struct {
    // This map's type is a placeholder, it's dynamically set
    // in rbperf.rs to either perf/ring buffer depending on
    // the configuration.
    __uint(type, BPF_MAP_TYPE_RINGBUF);
} events SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PROG_ARRAY);
    __uint(max_entries, 3);
    __type(key, u32);
    __type(value, u32);
} programs SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10240);
    __type(key, u32);
    __type(value, ProcessData);
} pid_to_rb_thread SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10240);
    __type(key, u32);
    __type(value, RubyFrame);
} id_to_stack SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10240);
    __type(key, RubyFrame);
    __type(value, u32);
} stack_to_id SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 10);
    __type(key, u32);
    __type(value, RubyVersionOffsets);
} version_specific_offsets SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, SampleState);
} global_state SEC(".maps");

const volatile bool verbose = false;
const volatile bool use_ringbuf = false;
const volatile bool enable_pid_race_detector = true;
const volatile enum rbperf_event_type event_type = RBPERF_EVENT_SYSCALL_UNKNOWN;

#define LOG(fmt, ...)                       \
    ({                                      \
        if (verbose) {                      \
            bpf_printk(fmt, ##__VA_ARGS__); \
        }                                   \
    })

static inline_method int read_syscall_id(void *ctx, int *syscall_id) {
    return bpf_probe_read_kernel(syscall_id, SYSCALL_NR_SIZE, ctx + SYSCALL_NR_OFFSET);
}

static inline_method u32 find_or_insert_frame(RubyFrame *frame) {
    u32 *found_id = bpf_map_lookup_elem(&stack_to_id, frame);
    if (found_id != NULL) {
        return *found_id;
    }
    // TODO(javierhonduco): Instead of calling the random number generator
    // we could generate unique IDs per CPU.
    u32 random = bpf_get_prandom_u32();
    bpf_map_update_elem(&stack_to_id, frame, &random, BPF_ANY);
    bpf_map_update_elem(&id_to_stack, &random, frame, BPF_ANY);
    return random;
}

static inline_method void read_ruby_string(u64 label, char *buffer,
                                           int buffer_len) {
    u64 flags;
    u64 char_ptr;

    rbperf_read(&flags, 8, (void *)(label + 0 /* .basic */ + 0 /* .flags */));

    if (STRING_ON_HEAP(flags)) {
        rbperf_read(&char_ptr, 8,
                    (void *)(label + as_offset + 8 /* .long len */));
        int err = rbperf_read_str(buffer, buffer_len, (void *)(char_ptr));
        if (err < 0) {
            LOG("[warn] string @ 0x%llx [heap] failed with err=%d", (void *)(char_ptr), err);
        }
    } else {
        int err = rbperf_read_str(buffer, buffer_len, (void *)(label + as_offset));
        if (err < 0) {
            LOG("[warn] string @ 0x%llx [stack] failed with err=%d", (void *)(label + as_offset), err);
        }
    }
}

static inline_method int
read_ruby_lineno(u64 pc, u64 body, RubyVersionOffsets *version_offsets) {
    // This will only give accurate line number for Ruby 2.4

    u64 pos_addr;
    u64 pos;
    u64 info_table;
    u32 line_info_size;
    u32 lineno;

    // Native functions have 0 as pc
    if (pc == 0) {
        return 0;
    }

    rbperf_read(&pos_addr, 8, (void *)(pc - body + iseq_encoded_offset));
    rbperf_read(&pos, 8, (void *)pos_addr);

    if (pos != 0) {
        pos -= rb_value_sizeof;
    }

    rbperf_read(&line_info_size, 4,
                (void *)(body + version_offsets->line_info_size_offset));
    if (line_info_size == 0) {
        return line_info_size;
    } else {
        rbperf_read(
            &info_table, 8,
            (void *)(body + version_offsets->line_info_table_offset));
        rbperf_read(&lineno, 4,
                    (void *)(info_table + (line_info_size - 1) * 0x8 +
                             version_offsets->lineno_offset));
        return lineno;
    }
}

static inline_method void
read_frame(u64 pc, u64 body, RubyFrame *current_frame,
           RubyVersionOffsets *version_offsets) {
    u64 path_addr;
    u64 path;
    u64 label;
    u64 flags;
    int label_offset = version_offsets->label_offset;

    LOG("[debug] reading stack");

    rbperf_read(&path_addr, 8,
                (void *)(body + ruby_location_offset + path_offset));
    rbperf_read(&flags, 8, (void *)path_addr);
    if ((flags & RUBY_T_MASK) == RUBY_T_STRING) {
        path = path_addr;
    } else if ((flags & RUBY_T_MASK) == RUBY_T_ARRAY) {
        if (version_offsets->path_flavour == 1) {
            // sizeof(struct RBasic)
            path_addr = path_addr + 0x10 /* offset(..., as) */ + PATH_TYPE_OFFSET;
            rbperf_read(&path, 8, (void *)path_addr);
        } else {
            path = path_addr;
        }

    } else {
        LOG("[error] read_frame, wrong type");
        // Skip as we don't have the data types we were looking for
        return;
    }

    rbperf_read(&label, 8,
                (void *)(body + ruby_location_offset + label_offset));

    read_ruby_string(path, current_frame->path, sizeof(current_frame->path));
    current_frame->lineno = read_ruby_lineno(pc, body, version_offsets);
    read_ruby_string(label, current_frame->method_name,
                     sizeof(current_frame->method_name));

    LOG("[debug] method name=%s", current_frame->method_name);
}

SEC("perf_event")
int walk_ruby_stack(struct bpf_perf_event_data *ctx) {
    u64 iseq_addr;
    u64 pc;
    u64 pc_addr;
    u64 body;

    int zero = 0;
    SampleState *state = bpf_map_lookup_elem(&global_state, &zero);
    if (state == NULL) {
        return 0;  // this should never happen
    }
    RubyVersionOffsets *version_offsets = bpf_map_lookup_elem(&version_specific_offsets, &state->rb_version);
    if (version_offsets == NULL) {
        return 0;  // this should not happen
    }

    RubyFrame current_frame = {};
    u64 base_stack = state->base_stack;
    u64 cfp = state->cfp;
    state->ruby_stack_program_count += 1;
    u64 control_frame_t_sizeof = version_offsets->control_frame_t_sizeof;

#pragma unroll
    for (int i = 0; i < MAX_STACKS_PER_PROGRAM; i++) {
        rbperf_read(&iseq_addr, 8, (void *)(cfp + iseq_offset));
        rbperf_read(&pc_addr, 8, (void *)(cfp + 0));
        rbperf_read(&pc, 8, (void *)pc_addr);

        if (cfp > state->base_stack) {
            LOG("[debug] done reading stack");
            break;
        }

        if ((void *)iseq_addr == NULL) {
            // this could be a native frame, it's missing the check though
            // https://github.com/ruby/ruby/blob/4ff3f20/.gdbinit#L1155
            // TODO(javierhonduco): Fetch path for native stacks
            bpf_probe_read_kernel_str(current_frame.method_name, sizeof(NATIVE_METHOD_NAME), NATIVE_METHOD_NAME);
        } else {
            rbperf_read(&body, 8, (void *)(iseq_addr + body_offset));
            read_frame(pc, body, &current_frame, version_offsets);
        }

        long long int actual_index = state->stack.size;
        if (actual_index >= 0 && actual_index < MAX_STACK) {
            state->stack.frames[actual_index] = find_or_insert_frame(&current_frame);
            state->stack.size += 1;
        }

        cfp += control_frame_t_sizeof;
    }

    state->cfp = cfp;
    state->base_stack = base_stack;

    if (cfp <= base_stack &&
        state->ruby_stack_program_count < BPF_PROGRAMS_COUNT) {
        LOG("[debug] traversing next chunk of the stack in a tail call");
        bpf_tail_call(ctx, &programs, RBPERF_STACK_READING_PROGRAM_IDX);
    }

    state->stack.stack_status = cfp > state->base_stack ? STACK_COMPLETE : STACK_INCOMPLETE;

    if (state->stack.size != state->stack.expected_size) {
        LOG("[error] stack size %d, expected %d", state->stack.size, state->stack.expected_size);
    }

    if (use_ringbuf) {
        bpf_ringbuf_output(&events, &state->stack, sizeof(RubyStack), 0);
    } else {
        bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU, &state->stack, sizeof(RubyStack));
    }
    return 0;
}

SEC("perf_event")
int on_event(struct bpf_perf_event_data *ctx) {
    u64 pid_tgid = bpf_get_current_pid_tgid();
    u32 pid = pid_tgid >> 32;
    ProcessData *process_data = bpf_map_lookup_elem(&pid_to_rb_thread, &pid);

    if (process_data != NULL && process_data->rb_frame_addr != 0) {
        LOG("[debug] reading Ruby stack");

        struct task_struct *task = (void *)bpf_get_current_task();
        if (task == NULL) {
            LOG("[error] task_struct was NULL");
            return 0;
        }

        // PIDs in Linux are reused. To ensure that the process we are
        // profiling is the one we expect, we check the pid + start_time
        // of the process.
        //
        // When we start profiling, the start_time will be zero, so we set
        // it to the actual start time. Otherwise, we check that the start_time
        // of the process matches what we expect. If it's not the case, bail out
        // early, to avoid profiling the wrong process.
        if (enable_pid_race_detector) {
            u64 process_start_time;
            bpf_core_read(&process_start_time, 8, &task->start_time);

            if (process_data->start_time == 0) {
                // First time seeing this process
                process_data->start_time = process_start_time;
            } else {
                // Let's check that the start time matches what we saw before
                if (process_data->start_time != process_start_time) {
                    LOG("[error] the process has probably changed...");
                    return 0;
                }
            }
        }

        u64 ruby_current_thread_addr;
        u64 main_thread_addr;
        u64 ec_addr;
        u64 thread_stack_content;
        u64 thread_stack_size;
        u64 cfp;
        int control_frame_t_sizeof;
        RubyVersionOffsets *version_offsets = bpf_map_lookup_elem(&version_specific_offsets, &process_data->rb_version);

        if (version_offsets == NULL) {
            LOG("[error] can't find offsets for version");
            return 0;
        }

        rbperf_read(&ruby_current_thread_addr, 8,
                    (void *)process_data->rb_frame_addr);

        LOG("process_data->rb_frame_addr 0x%llx", process_data->rb_frame_addr);
        LOG("ruby_current_thread_addr 0x%llx", ruby_current_thread_addr);

        // Find the main thread and the ec
        rbperf_read(&main_thread_addr, 8,
                    (void *)ruby_current_thread_addr + version_offsets->main_thread_offset);
        rbperf_read(&ec_addr, 8, (void *)main_thread_addr + version_offsets->ec_offset);

        control_frame_t_sizeof = version_offsets->control_frame_t_sizeof;

        rbperf_read(
            &thread_stack_content, 8,
            (void *)(ec_addr + version_offsets->vm_offset));
        rbperf_read(
            &thread_stack_size, 8,
            (void *)(ec_addr + version_offsets->vm_size_offset));

        u64 base_stack = thread_stack_content +
                         rb_value_sizeof * thread_stack_size -
                         2 * control_frame_t_sizeof /* skip dummy frames */;
        rbperf_read(&cfp, 8, (void *)(ec_addr + version_offsets->cfp_offset));
        int zero = 0;
        SampleState *state = bpf_map_lookup_elem(&global_state, &zero);
        if (state == NULL) {
            return 0;  // this should never happen
        }

        // Set the global state, shared across bpf tail calls
        state->stack.timestamp = bpf_ktime_get_ns();
        state->stack.pid = pid;
        state->stack.cpu = bpf_get_smp_processor_id();
        if (event_type == RBPERF_EVENT_SYSCALL) {
            read_syscall_id(ctx, &state->stack.syscall_id);
        } else {
            state->stack.syscall_id = 0;
        }
        state->stack.size = 0;
        state->stack.expected_size = (base_stack - cfp) / control_frame_t_sizeof;
        bpf_get_current_comm(state->stack.comm, sizeof(state->stack.comm));
        state->stack.stack_status = STACK_COMPLETE;

        state->base_stack = base_stack;
        state->cfp = cfp + version_offsets->control_frame_t_sizeof;
        state->ruby_stack_program_count = 0;
        state->rb_version = process_data->rb_version;

        bpf_tail_call(ctx, &programs, RBPERF_STACK_READING_PROGRAM_IDX);
        // This will never be executed
        return 0;
    }
    return 0;
}

char LICENSE[] SEC("license") = "Dual MIT/GPL";

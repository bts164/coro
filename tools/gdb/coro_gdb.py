# gdb commands for inspecting a running coro program: `source tools/gdb/coro_gdb.py` in gdb. They read
# libstdc++ types through its pretty printers, so those must be loaded (`info pretty-printer` lists them).
#
# coro-tasks [EXECUTOR]
#     Lists every task in a WorkStealingExecutor's owned set (detail::OwnedTasks): one line per task with
#     its shard, address, kind, name, scheduling state and cancel flags. EXECUTOR is any expression for the
#     executor or a pointer to it; the default is the current thread's coro::t_wse_owning_executor, so
#     select a worker thread first (`thread N`), or pass `*this` from a frame inside a WorkStealingExecutor
#     method.
#
# coro-awaiting TASK
#     Follows what a task is suspended on, from its top-level future down to the leaf: for each Coro or
#     CoroStream on the way, the coroutine (by its resume function) and the future its promise is awaiting
#     (m_poll_current). Prints the leaf future in full. TASK is a task's address, as coro-tasks prints it.
import re

import gdb


def first_pointer_field(val, depth=0):
    """The first raw pointer inside val (e.g. a unique_ptr's), searching its fields depth first."""
    if val.type.strip_typedefs().code == gdb.TYPE_CODE_PTR:
        return val
    if depth > 8 or val.type.strip_typedefs().code not in (gdb.TYPE_CODE_STRUCT, gdb.TYPE_CODE_UNION):
        return None
    for f in val.type.strip_typedefs().fields():
        try:
            found = first_pointer_field(val[f], depth + 1)
        except gdb.error:
            continue
        if found is not None:
            return found
    return None


def try_field(val, name):
    """val[name] as text, without std::atomic's wrapping or an enum value's qualification; None if absent."""
    try:
        text = str(val[name])
    except gdb.error:
        return None
    m = re.fullmatch(r"std::atomic<.*> = \{ (.*) \}", text, re.DOTALL)
    if m:
        text = m.group(1)
    return text.rsplit("::", 1)[-1] if re.fullmatch(r"[\w:]+", text) else text


def short_type(t):
    name = str(t.target() if t.code == gdb.TYPE_CODE_PTR else t)
    return name.split("<", 1)[0].rsplit("::", 1)[-1]


class CoroTasks(gdb.Command):
    """List the tasks in a coro WorkStealingExecutor: coro-tasks [EXECUTOR]"""

    def __init__(self):
        super().__init__("coro-tasks", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        ex = gdb.parse_and_eval(arg or "coro::t_wse_owning_executor")
        if ex.type.strip_typedefs().code == gdb.TYPE_CODE_PTR:
            if int(ex) == 0:
                raise gdb.GdbError("no executor on this thread: select a worker thread, or pass one")
            ex = ex.dereference()
        owned = ex["m_owned_tasks"]
        mask = int(owned["m_shard_mask"])
        shards = first_pointer_field(owned["m_shards"])
        count = 0
        for i in range(mask + 1):
            task = shards[i]["head"]
            while int(task) != 0:
                count += 1
                dyn = task.dynamic_type
                concrete = task.cast(dyn).dereference()
                flags = []
                for field in ("cancel_requested", "cancelled", "m_cancelled", "closed", "terminated"):
                    v = try_field(concrete, field)
                    if v is not None:
                        flags.append(f"{field}={v}")
                print(f"[{i:3}] {int(task):#x} {short_type(dyn):<16} {try_field(concrete, 'name') or '?':<20} "
                      f"state={try_field(concrete, 'scheduling_state')} {' '.join(flags)}")
                task = task["owned_next"]
        print(f"{count} task(s) in {mask + 1} shard(s)")


CoroTasks()


def deref(val):
    """val with any reference stripped."""
    return val.referenced_value() if val.type.code in (gdb.TYPE_CODE_REF, gdb.TYPE_CODE_RVALUE_REF) else val


def find_member(val, name, depth=0):
    """The first field called name in val, searching its fields (and std::optional's payload) depth first."""
    val = deref(val)
    if depth > 8 or val.type.strip_typedefs().code not in (gdb.TYPE_CODE_STRUCT, gdb.TYPE_CODE_UNION):
        return None
    for f in val.type.strip_typedefs().fields():
        if f.name == name:
            return val[f]
    for f in val.type.strip_typedefs().fields():
        try:
            found = find_member(val[f], name, depth + 1)
        except gdb.error:
            continue
        if found is not None:
            return found
    return None


def is_coroutine_handle(t):
    # libstdc++ spells it std::__n4861::coroutine_handle<P>.
    return re.match(r"std::(\w+::)?coroutine_handle<", str(t.strip_typedefs())) is not None


def coroutine_handle(fut):
    """The std::coroutine_handle a Coro or CoroStream holds, or that a future borrowing one (next(stream))
    reaches through a reference or pointer field; None for any other future."""
    fields = fut.type.strip_typedefs().fields() if fut.type.strip_typedefs().code == gdb.TYPE_CODE_STRUCT else []
    for f in fields:
        if f.name == "m_handle" and is_coroutine_handle(f.type):
            return fut[f], fut
    for f in fields:
        code = f.type.strip_typedefs().code
        if code in (gdb.TYPE_CODE_REF, gdb.TYPE_CODE_RVALUE_REF, gdb.TYPE_CODE_PTR) and not f.artificial:
            target = deref(fut[f]) if code != gdb.TYPE_CODE_PTR else (
                fut[f].dereference() if int(fut[f]) else None)
            if target is not None and target.type.strip_typedefs().code == gdb.TYPE_CODE_STRUCT:
                for g in target.type.strip_typedefs().fields():
                    if g.name == "m_handle" and is_coroutine_handle(g.type):
                        return target[g], target
    return None


def symbol_at(addr):
    try:
        return str(gdb.parse_and_eval(f"(void(*)()){addr:#x}")).split("<", 1)[-1].rstrip(">")
    except gdb.error:
        return f"{addr:#x}"


class CoroAwaiting(gdb.Command):
    """Follow what a coro task is suspended on, down to the leaf future: coro-awaiting TASK"""

    def __init__(self):
        super().__init__("coro-awaiting", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        if not arg:
            raise gdb.GdbError("usage: coro-awaiting TASK (an address from coro-tasks)")
        task = gdb.parse_and_eval(f"(coro::detail::TaskBase*)({arg})")
        concrete = task.cast(task.dynamic_type).dereference()
        print(f"task {int(task):#x} {short_type(task.dynamic_type)} {try_field(concrete, 'name')}")
        # StreamTaskImpl runs a stream, TaskImpl (and JoinSetTask) a future; each in a std::optional.
        fut = find_member(concrete, "m_stream" if short_type(task.dynamic_type) == "StreamTaskImpl" else "m_future")
        if fut is None:
            raise gdb.GdbError("the task has no m_stream or m_future")
        value = find_member(fut, "_M_value")  # std::optional's payload
        if value is not None:
            fut = value
        for level in range(32):
            fut = deref(fut)
            found = coroutine_handle(fut)
            if found is None:
                print(f"{'  ' * level}leaf: {fut.type}")
                print(fut)
                return
            handle, fut = found
            frame = int(handle["_M_fr_ptr"])
            draining = try_field(fut, "m_cancel_draining")
            cancelled = try_field(fut, "m_cancelled")
            if frame == 0:
                print(f"{'  ' * level}{short_type(fut.type)}: no frame (finished or destroyed)")
                return
            print(f"{'  ' * level}{short_type(fut.type)} frame {frame:#x}: "
                  f"{symbol_at(int(gdb.parse_and_eval(f'*(void**){frame:#x}')))}"
                  + (f" cancelled={cancelled}" if cancelled is not None else "")
                  + (f" cancel_draining={draining}" if draining is not None else ""))
            # GCC lays a coroutine frame out as the resume and destroy pointers, then the promise.
            promise_type = handle.type.strip_typedefs().template_argument(0)
            promise = gdb.parse_and_eval(f"*({promise_type}*)({frame:#x} + 16)")
            current = promise["m_poll_current"]
            if int(current) == 0:
                print(f"{'  ' * level}  not awaiting a future: at a co_yield, or a plain awaiter")
                return
            awaitable = current.cast(current.dynamic_type).dereference()
            print(f"{'  ' * level}  awaiting {short_type(current.dynamic_type)}<{awaitable['m_future'].type}>")
            fut = awaitable["m_future"]
        print("stopped after 32 levels")


CoroAwaiting()

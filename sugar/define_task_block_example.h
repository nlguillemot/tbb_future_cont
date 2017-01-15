// Implementation of define_task_block using cont stuff.
// Just kept around as an example.

#include <tbb/task.h>

#include <iostream>
#include <sstream>
#include <cassert>
#include <atomic>
#include <array>
#include <thread>
#include <mutex>
#include <random>

// node in a linked list of tasks that depend on a cont
struct cont_node
{
    tbb::task* task;
    cont_node* next;
};

// base class for working with conts (encapsulates tricky atomic code)
class cont_base
{
    // head of the linked list of successors queued on this cont
    std::atomic<cont_node*> _head = NULL;

public:
    cont_base() = default;

    cont_base(const cont_base&) = delete;
    cont_base& operator=(const cont_base&) = delete;
    cont_base(cont_base&&) = delete;
    cont_base& operator=(cont_base&&) = delete;

    // return true if this cont has been set_ready()
    bool is_ready() const
    {
        return ((intptr_t)_head.load(std::memory_order_acquire) & 1) != 0;
    }

    // sends this cont to all successors in the linked list.
    void set_ready()
    {
        assert(!is_ready());

        cont_node* old_head;

        // mark the cont as ready atomically. readiness is indicated by the least significant bit of the head pointer.
        for (;;)
        {
            old_head = _head.load(std::memory_order_acquire);
            cont_node* ready_head = (cont_node*)((intptr_t)old_head | 1);

            if (_head.compare_exchange_weak(old_head, ready_head, std::memory_order_acq_rel, std::memory_order_relaxed))
            {
                // If the CAS failed, that means a successor just added themselves to the list,
                // since that successor thought this cont was not ready yet. (or possibly it was a spurious wakeup.)
                // If it succeeded, then the cont can notify all successors that have been queued so far.
                break;
            }
        }

        // Notify all successors that have been queued
        for (cont_node* node = old_head; node != NULL; node = node->next)
        {
            if (node->task->decrement_ref_count() == 0)
            {
                // this was the last missing input, so the task can now be spawned.
                tbb::task::spawn(*node->task);
            }
        }
    }

    // Tries adding the given task to the cont's successor linked list using the given linked list node.
    // This fails (and returns false) if the successor queue has already been closed because the cont has already been set.
    // If it succeeds (and returns true), then the passed-in task was successfully added to the linked list.
    bool try_register_successor(tbb::task* t, cont_node* c)
    {
        cont_node* new_head = c;
        new_head->task = t;

        for (;;)
        {
            cont_node* old_head = _head.load(std::memory_order_acquire);

            if ((intptr_t)old_head & 1)
            {
                // cont was already set, so can't register yourself.
                // the caller should use this knowledge to know that they can just read from the cont without queueing themselves.
                return false;
            }

            new_head->next = old_head;

            // It's possible for the successor notification queue to be closed concurrently while we're trying to add ourselves to it.
            // It's also possible for another successor to have registered themselves concurrently and beat this successor to the punch.
            if (_head.compare_exchange_weak(old_head, new_head, std::memory_order_acq_rel, std::memory_order_relaxed))
            {
                return true;
            }
        }
    }
};

// Associates data to a cont, std::optional-style.
// TODO: Just replace all of this with std::optional?
template<class T>
class cont : public cont_base
{
    bool _has_value = false;
    std::aligned_storage_t<sizeof(T), alignof(T)> _storage;

public:
    cont() = default;

    cont(const cont&) = delete;
    cont& operator=(const cont&) = delete;
    cont(cont&&) = delete;
    cont& operator=(cont&&) = delete;

    ~cont()
    {
        if (_has_value)
        {
            (**this).~T();
        }
    }

    T* operator->()
    {
        return reinterpret_cast<T*>(&_storage);
    }

    const T* operator->() const
    {
        return reinterpret_cast<const T*>(&_storage);
    }

    T& operator*()
    {
        return *reinterpret_cast<T*>(&_storage);
    }

    const T& operator*() const
    {
        return *reinterpret_cast<const T*>(&_storage);
    }

    template<class... Args>
    void emplace(Args&&... args)
    {
        assert(!is_ready());

        if (_has_value)
        {
            (**this).~T();
        }

        new (&_storage) T(std::forward<Args>(args)...);

        _has_value = true;
    }
};

// spawns the given task when all the "conts" are ready. There must be a linked list node supplied for each cont.
void spawn_when_ready(tbb::task& t, cont_base** conts, cont_node* nodes, int num_conts)
{
    // +1 reference count for each missing argument
    // the task is only spawned when the reference count is zero,
    // so that means it gets decremented once for each input that gets filled in.
    t.add_ref_count(num_conts);

    int num_inputs_already_ok = 0;

    for (size_t cont_i = 0; cont_i < num_conts; cont_i++)
    {
        cont_base* c = conts[cont_i];

        // try registering the task as a successor of each cont, so the task will get notified (and its refcount decremented) when the cont becomes available
        if (!c->try_register_successor(&t, &nodes[cont_i]))
        {
            // if we can't subscribe a successor to the cont, that means the cont is already set.
            // in other words, that input is already ready to go, and we don't need to wait for a notification about it.
            num_inputs_already_ok++;
        }
    }

    // incorporate the inputs that already okay into the reference count.
    // if the reference count hits zero, that means all inputs are satisfied and the task can be spawned.
    if (num_inputs_already_ok > 0)
    {
        if (t.add_ref_count(-num_inputs_already_ok) == 0)
        {
            tbb::task::spawn(t);
        }
    }
}

class task_block
{
    tbb::task* _self;

    task_block(tbb::task* self)
        : _self(self)
    { }

    template<class TaskFun, bool WaitForAll = false>
    class task_runner : public tbb::task
    {
        TaskFun mfun;

    public:
        explicit task_runner(TaskFun& fun)
            : mfun(fun)
        { }

        tbb::task* execute() override
        {
            mfun();
            if (WaitForAll)
            {
                increment_ref_count();
                wait_for_all();
            }
            return NULL;
        }
    };

    template<class TaskFun, int NumConts>
    class cont_task_runner : public tbb::task
    {
        TaskFun mfun;

    public:
        std::array<cont_base*, NumConts> conts;
        std::array<cont_node, NumConts> nodes;

        explicit cont_task_runner(TaskFun& fun)
            : mfun(fun)
        { }

        tbb::task* execute() override
        {
            mfun();
            return NULL;
        }
    };

public:
    task_block(const task_block&) = delete;
    task_block& operator=(const task_block&) = delete;
    task_block* operator&() const = delete;

    template<class TaskFun>
    void run(TaskFun&& tfun)
    {
        _self->spawn(*new (_self->allocate_additional_child_of(*_self)) task_runner<TaskFun>(tfun));
    }

    template<int NumConts>
    class with_spawner
    {
        task_block* tb;
        std::array<cont_base*, NumConts> conts;

    public:
        friend class task_block;

        template<class TaskFun>
        void run(TaskFun&& tfun)
        {
            auto& t = *new (tb->_self->allocate_additional_child_of(*tb->_self)) cont_task_runner<TaskFun, NumConts>(tfun);
            t.conts = conts;
            spawn_when_ready(t, t.conts.data(), t.nodes.data(), (int)t.conts.size());
        }
    };

    template<class... Cont>
    auto with(Cont&... conts)
    {
        with_spawner<sizeof...(conts)> spawner;
        spawner.tb = this;
        spawner.conts = { (&conts)... };
        return spawner;
    }

    void wait()
    {
        _self->increment_ref_count();
        _self->wait_for_all();
    }

    tbb::task& task()
    {
        return *_self;
    }

    template<class TBlockFun>
    static void define_task_block(TBlockFun&& tbfun)
    {
        task_block tb(&tbb::task::self());
        tbfun(tb);
        tb._self->increment_ref_count();
        tb._self->wait_for_all();
    }

    template<class TBlockFun>
    static void define_root_task_block(TBlockFun&& tbfun)
    {
        task_block tb(0);
        auto task_fun = [&] { tbfun(tb); };
        tb._self = new (tbb::task::allocate_root()) task_runner<decltype(task_fun), true>(task_fun);
        tbb::task::spawn_root_and_wait(*tb._self);
    }
};

template<class TBlockFun>
void define_task_block(TBlockFun tbfun)
{
    task_block::define_task_block(tbfun);
}

template<class TBlockFun>
void define_root_task_block(TBlockFun tbfun)
{
    task_block::define_root_task_block(tbfun);
}

// wait for a random number of milliseconds, used to test the system with varying timings.
void random_wait()
{
    static std::mt19937 e = std::mt19937(std::random_device()());
    static std::mutex e_lock;

    std::uniform_int_distribution<int> dist(1, 5000);

    e_lock.lock();
    std::chrono::milliseconds wait_time(dist(e));
    e_lock.unlock();

    std::this_thread::sleep_for(wait_time);
}

void TaskA(cont<int>* c, int x)
{
    std::cout << "TaskA start\n";

    random_wait();

    define_task_block([&](task_block& tb) {
        tb.run([&] {
            std::cout << "A Subtask 1 start\n";
            random_wait();
            c->emplace(1337);
            c->set_ready();
            std::cout << "A Subtask 1 end\n";
        });
        tb.run([&] {
            std::cout << "A Subtask 2 start\n";
            random_wait();
            std::cout << "A Subtask 2 end\n";
        });
    });

    std::cout << "TaskA end\n";
}

void TaskB(int y)
{
    std::cout << "TaskB start\n";
    random_wait();
    std::cout << "TaskB end\n";
}

void TaskC(int z)
{
    std::cout << "TaskC start\n";

    std::stringstream ss;
    ss << "TaskC received " << z << "\n";
    std::cout << ss.rdbuf();

    random_wait();

    std::cout << "TaskC end\n";
}

int main()
{
    cont<int> c;

    define_root_task_block([&](task_block& tb) {
        tb.run([&] { TaskA(&c, 3); });
        tb.run([&] { TaskB(2); });
        tb.with(c).run([&] { TaskC(*c); });
    });

    system("pause");
}

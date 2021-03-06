#include <tbb/task.h>

#include <iostream>
#include <sstream>
#include <cassert>
#include <atomic>
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
    if (t.add_ref_count(-num_inputs_already_ok) == 0)
    {
        tbb::task::spawn(t);
    }
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

class TaskA : public tbb::task
{
    class A_Subtask1 : public tbb::task
    {
        cont<int>* c;

    public:
        A_Subtask1(cont<int>* c)
        {
            this->c = c;
        }

        tbb::task* execute() override
        {
            std::cout << "A_Subtask1 start\n";
            random_wait();

            // set the value of c
            c->emplace(1337);

            // broadcast that c is ready to all the successors enqueued on the cont.
            c->set_ready();

            std::cout << "A_Subtask1 end\n";

            return NULL;
        }
    };

    class A_Subtask2 : public tbb::task
    {
    public:
        tbb::task* execute() override
        {
            std::cout << "A_Subtask2 start\n";
            random_wait();
            std::cout << "A_Subtask2 end\n";

            return NULL;
        }
    };

    cont<int>* c;

public:
    TaskA(cont<int>* c)
    {
        this->c = c;
    }

    tbb::task* execute() override
    {
        std::cout << "A start\n";

        // 2 children + wait
        set_ref_count(3);

        spawn(*new(allocate_child()) A_Subtask1(c));
        spawn_and_wait_for_all(*new(allocate_child()) A_Subtask2());

        std::cout << "A end\n";

        return NULL;
    }
};

class TaskB : public tbb::task
{
public:
    tbb::task* execute() override
    {
        std::cout << "B start\n";
        random_wait();
        std::cout << "B end\n";

        return NULL;
    }
};

class TaskC : public tbb::task
{
    cont<int>* c;

public:
    cont_node cnodes[1];

    TaskC(cont<int>* c)
    {
        this->c = c;
    }

    tbb::task* execute() override
    {
        std::cout << "C start\n";

        std::stringstream ss;
        ss << "C received " << **c << "\n";
        std::cout << ss.rdbuf();

        std::cout << "C end\n";

        return NULL;
    }
};

class MainTask : public tbb::task
{
public:
    tbb::task* execute() override
    {
        // this variable will be produced by a subtask of task A, and consumed by task C.
        cont<int> c;

        // 3 children + wait
        set_ref_count(4);

        spawn(*new(allocate_child()) TaskA(&c));

        random_wait();

        spawn(*new(allocate_child()) TaskB());

        random_wait();

        // run TaskC when all the conts are satisfied (cont "c" gets set inside TaskA)
        TaskC& taskC = *new(allocate_child()) TaskC(&c);
        cont_base* conts[] = { &c };
        spawn_when_ready(taskC, conts, taskC.cnodes, _countof(conts));

        wait_for_all();

        return NULL;
    }
};

int main()
{
    tbb::task::spawn_root_and_wait(*new(tbb::task::allocate_root()) MainTask());
    system("pause");
}

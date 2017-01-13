#include <tbb/task.h>

#include <iostream>
#include <sstream>
#include <cassert>
#include <atomic>

struct cont_node
{
    tbb::task* task;
    cont_node* next;
};

class cont_base
{
    std::atomic<cont_node*> _head = NULL;

public:
    cont_base() = default;

    cont_base(const cont_base&) = delete;
    cont_base& operator=(const cont_base&) = delete;
    cont_base(cont_base&&) = delete;
    cont_base& operator=(cont_base&&) = delete;

    bool is_ready() const
    {
        return ((intptr_t)_head.load() & 1) != 0;
    }

    void set_ready()
    {
        assert(!is_ready());

        cont_node* old_head;

        // mark the cont as ready atomically
        for (;;)
        {
            // TODO: pick better than memory_order_seq_cst
            old_head = _head.load(std::memory_order_seq_cst);
            cont_node* ready_head = (cont_node*)((intptr_t)old_head | 1);

            // TODO: pick better than memory_order_seq_cst and consider if I should use _explicit
            if (_head.compare_exchange_weak(old_head, ready_head, std::memory_order_seq_cst))
            {
                // If the CAS failed, that means a successor just added themselves to the list,
                // since that successor thought this cont was not ready yet.
                // If it succeeded, then the cont can notify all successors that have been queued so far.
                break;
            }
        }

        // Notify all successors that have been queued
        for (cont_node* node = old_head; node != NULL; node = node->next)
        {
            if (node->task->decrement_ref_count() == 0)
            {
                tbb::task* parent = (*node->task).parent();
                if (parent)
                {
                    parent->add_ref_count(1);
                }

                tbb::task::spawn(*node->task);
            }
        }
    }

    bool try_register_successor(tbb::task* t, cont_node* c)
    {
        cont_node* new_head = c;
        new_head->task = t;

        // TODO: pick better than memory_order_seq_cst
        cont_node* old_head = _head.load(std::memory_order_seq_cst);
        new_head->next = old_head;

        // TODO: pick better than memory_order_seq_cst
        return _head.compare_exchange_strong(old_head, new_head, std::memory_order_seq_cst);
    }
};

// TODO: Just replace all of this with std::optional?
template<class T>
class cont : public cont_base
{
    bool _has_value = false;
    std::aligned_storage_t<sizeof(T), alignof(T)> _storage;

public:
    explicit cont(bool default_emplace = true)
    {
        if (default_emplace)
        {
            emplace();
        }
    }

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
        return *reinterpret_cast<const T*>(&_storage);
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

void spawn_when_ready(tbb::task& t, cont_base** conts, cont_node* nodes, int num_conts)
{
    t.add_ref_count(num_conts);

    bool any_missing = false;

    for (size_t cont_i = 0; cont_i < num_conts; cont_i++)
    {
        cont_base* c = conts[cont_i];
        
        if (!c->try_register_successor(&t, &nodes[cont_i]))
        {
            t.decrement_ref_count();
        }
        else
        {
            any_missing = true;
        }
    }
    
    if (!any_missing)
    {
        tbb::task* parent = t.parent();
        if (parent)
        {
            parent->add_ref_count(1);
        }
        
        tbb::task::spawn(t);
    }
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
            std::cout << "A_Subtask1\n";

            c->emplace(1337);
            c->set_ready();
            return NULL;
        }
    };

    class A_Subtask2 : public tbb::task
    {
    public:
        tbb::task* execute() override
        {
            std::cout << "A_Subtask2\n";

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
        std::cout << "A\n";

        set_ref_count(3);
        spawn(*new(allocate_child()) A_Subtask1(c));
        spawn_and_wait_for_all(*new(allocate_child()) A_Subtask2());
        return NULL;
    }
};

class TaskB : public tbb::task
{
public:
    tbb::task* execute() override
    {
        std::cout << "B\n";

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
        std::cout << "C\n";

        std::stringstream ss;
        ss << "C received " << **c << "\n";
        std::cout << ss.rdbuf();
        return NULL;
    }
};

class MainTask : public tbb::task
{
public:
    tbb::task* execute() override
    {
        cont<int> c;

        // 3 children + wait
        set_ref_count(4);

        spawn(*new(allocate_child()) TaskA(&c));
        spawn(*new(allocate_child()) TaskB());

        TaskC& taskC = *new(allocate_child()) TaskC(&c);
        cont_base* conts[] = { &c };
        spawn_when_ready(taskC, conts, taskC.cnodes, _countof(conts));

        spawn_and_wait_for_all(*new (allocate_child()) tbb::empty_task());

        return NULL;
    }
};

int main()
{
    tbb::task::spawn_root_and_wait(*new(tbb::task::allocate_root()) MainTask());
}
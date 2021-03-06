/*
 * \brief  Singlethreaded minimalistic kernel
 * \author Martin Stein
 * \date   2011-10-20
 *
 * This kernel is the only code except the mode transition PIC, that runs in
 * privileged CPU mode. It has two tasks. First it initializes the process
 * 'core', enriches it with the whole identically mapped address range,
 * joins and applies it, assigns one thread to it with a userdefined
 * entrypoint (the core main thread) and starts this thread in userland.
 * Afterwards it is called each time an exception occurs in userland to do
 * a minimum of appropriate exception handling. Thus it holds a CPU context
 * for itself as for any other thread. But due to the fact that it never
 * relies on prior kernel runs this context only holds some constant pointers
 * such as SP and IP.
 */

/*
 * Copyright (C) 2011-2013 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

/* Genode includes */
#include <base/signal.h>
#include <cpu/cpu_state.h>
#include <base/thread_state.h>

/* core includes */
#include <platform_thread.h>
#include <tlb.h>
#include <trustzone.h>

using namespace Kernel;

/* get core configuration */
extern Genode::Native_utcb * _main_utcb;
extern int _kernel_stack_high;
extern "C" void CORE_MAIN();

/* get structure of mode transition PIC */
extern int _mode_transition_begin;
extern int _mode_transition_end;
extern int _mt_user_entry_pic;
extern int _mon_vm_entry;
extern Genode::addr_t _mt_client_context_ptr;
extern Genode::addr_t _mt_master_context_begin;
extern Genode::addr_t _mt_master_context_end;

namespace Kernel
{
	/* import Genode types */
	typedef Genode::Thread_state Thread_state;
	typedef Genode::umword_t umword_t;
}


void Kernel::Ipc_node::_receive_request(Message_buf * const r)
{
	/* assertions */
	assert(r->size <= _inbuf.size);

	/* fetch message */
	Genode::memcpy(_inbuf.base, r->base, r->size);
	_inbuf.size = r->size;
	_inbuf.origin = r->origin;

	/* update state */
	_state = r->origin->_awaits_reply() ? PREPARE_REPLY :
	                                      INACTIVE;
}


void Kernel::Ipc_node::_receive_reply(void * const base, size_t const size)
{
	/* assertions */
	assert(_awaits_reply());
	assert(size <= _inbuf.size);

	/* receive reply */
	Genode::memcpy(_inbuf.base, base, size);
	_inbuf.size = size;

	/* update state */
	if (_state != PREPARE_AND_AWAIT_REPLY) _state = INACTIVE;
	else _state = PREPARE_REPLY;
	_has_received(_inbuf.size);
}


void Kernel::Ipc_node::_announce_request(Message_buf * const r)
{
	/* directly receive request if we've awaited it */
	if (_state == AWAIT_REQUEST) {
		_receive_request(r);
		_has_received(_inbuf.size);
		return;
	}
	/* cannot receive yet, so queue request */
	_request_queue.enqueue(r);
}


bool Kernel::Ipc_node::_awaits_reply()
{
	return _state == AWAIT_REPLY ||
	       _state == PREPARE_AND_AWAIT_REPLY;
}


Kernel::Ipc_node::Ipc_node() : _state(INACTIVE)
{
	_inbuf.size = 0;
	_outbuf.size = 0;
}


void Kernel::Ipc_node::send_request_await_reply(Ipc_node * const dest,
                                                void * const     req_base,
                                                size_t const     req_size,
                                                void * const     inbuf_base,
                                                size_t const     inbuf_size)
{
	/* assertions */
	assert(_state == INACTIVE || _state == PREPARE_REPLY);

	/* prepare transmission of request message */
	_outbuf.base = req_base;
	_outbuf.size = req_size;
	_outbuf.origin = this;

	/* prepare reception of reply message */
	_inbuf.base = inbuf_base;
	_inbuf.size = inbuf_size;

	/* update state */
	if (_state != PREPARE_REPLY) _state = AWAIT_REPLY;
	else _state = PREPARE_AND_AWAIT_REPLY;
	_awaits_receipt();

	/* announce request */
	dest->_announce_request(&_outbuf);
}


void Kernel::Ipc_node::await_request(void * const inbuf_base,
                                     size_t const inbuf_size)
{
	/* assertions */
	assert(_state == INACTIVE);

	/* prepare receipt of request */
	_inbuf.base = inbuf_base;
	_inbuf.size = inbuf_size;

	/* if anybody already announced a request receive it */
	if (!_request_queue.empty()) {
		_receive_request(_request_queue.dequeue());
		_has_received(_inbuf.size);
		return;
	}
	/* no request announced, so wait */
	_state = AWAIT_REQUEST;
	_awaits_receipt();
}


void Kernel::Ipc_node::send_reply(void * const reply_base,
                                  size_t const reply_size)
{
	/* reply to the last request if we have to */
	if (_state == PREPARE_REPLY) {
		_inbuf.origin->_receive_reply(reply_base, reply_size);
		_state = INACTIVE;
	}
}


void Kernel::Ipc_node::send_note(Ipc_node * const dest,
                                 void * const     note_base,
                                 size_t const     note_size)
{
	/* assert preconditions */
	assert(_state == INACTIVE || _state == PREPARE_REPLY);

	/* announce request message, our state says: No reply needed */
	_outbuf.base = note_base;
	_outbuf.size = note_size;
	_outbuf.origin = this;
	dest->_announce_request(&_outbuf);
}


namespace Kernel
{
	class Schedule_context;

	/**
	 * Controls the mode transition code
	 *
	 * The code that switches between kernel/user mode must not exceed the
	 * smallest page size supported by the MMU. The Code must be position
	 * independent. This code has to be mapped in every PD, to ensure
	 * appropriate kernel invokation on CPU interrupts.
	 * This class controls the settings like kernel, user, and VM states
	 * that are handled by the mode transition PIC.
	 */
	struct Mode_transition_control
	{
		enum {
			SIZE_LOG2 = Tlb::MIN_PAGE_SIZE_LOG2,
			SIZE = 1 << SIZE_LOG2,
			VIRT_BASE = Cpu::EXCEPTION_ENTRY,
			VIRT_END = VIRT_BASE + SIZE,
			ALIGNM_LOG2 = SIZE_LOG2,
		};

		addr_t const _virt_user_entry;

		/**
		 * Constructor
		 */
		Mode_transition_control() :
			_virt_user_entry(VIRT_BASE + ((addr_t)&_mt_user_entry_pic -
			                 (addr_t)&_mode_transition_begin))
		{
			/* check if mode transition PIC fits into aligned region */
			addr_t const pic_begin = (addr_t)&_mode_transition_begin;
			addr_t const pic_end = (addr_t)&_mode_transition_end;
			size_t const pic_size = pic_end - pic_begin;
			assert(pic_size <= SIZE);

			/* check if kernel context fits into the mode transition */
			addr_t const kc_begin = (addr_t)&_mt_master_context_begin;
			addr_t const kc_end = (addr_t)&_mt_master_context_end;
			size_t const kc_size = kc_end - kc_begin;
			assert(sizeof(Cpu::Context) <= kc_size);
		}

		/**
		 * Fetch next kernelmode context
		 */
		void fetch_master_context(Cpu::Context * const c) {
			memcpy(&_mt_master_context_begin, c, sizeof(Cpu::Context)); }

		/**
		 * Page aligned physical base of the mode transition PIC
		 */
		addr_t phys_base() { return (addr_t)&_mode_transition_begin; }

		/**
		 * Jump to the usermode entry PIC
		 */
		void virt_user_entry() {
			((void(*)(void))_virt_user_entry)(); }
	};


	/**
	 * Static mode transition control
	 */
	static Mode_transition_control * mtc()
	{ static Mode_transition_control _object; return &_object; }

	/**
	 * Kernel object that represents a Genode PD
	 */
	class Pd : public Object<Pd, MAX_PDS>
	{
		Tlb * const _tlb;

		/* keep ready memory for size aligned extra costs at construction */
		enum { EXTRA_SPACE_SIZE = 2*Tlb::MAX_COSTS_PER_TRANSLATION };
		char _extra_space[EXTRA_SPACE_SIZE];

		public:

			/**
			 * Constructor
			 */
			Pd(Tlb * const t) : _tlb(t)
			{
				/* try to add translation for mode transition region */
				Page_flags::access_t const flags = Page_flags::mode_transition();
				unsigned const slog2 =
					tlb()->insert_translation(mtc()->VIRT_BASE,
					                          mtc()->phys_base(),
					                          mtc()->SIZE_LOG2, flags);

				/* extra space needed to translate mode transition region */
				if (slog2)
				{
					/* Get size aligned extra space */
					addr_t const es = (addr_t)&_extra_space;
					addr_t const es_end = es + sizeof(_extra_space);
					addr_t const aligned_es = (es_end - (1<<slog2)) &
					                          ~((1<<slog2)-1);
					addr_t const aligned_es_end = aligned_es + (1<<slog2);

					/* check attributes of aligned extra space */
					assert(aligned_es >= es && aligned_es_end <= es_end)

					/* translate mode transition region globally */
					tlb()->insert_translation(mtc()->VIRT_BASE,
					                          mtc()->phys_base(),
					                          mtc()->SIZE_LOG2, flags,
					                          (void *)aligned_es);
				}
			}

			/**
			 * Add the CPU context 'c' to this PD
			 */
			void append_context(Cpu::Context * const c)
			{
				c->protection_domain(id());
				c->tlb(tlb()->base());
			}

			/***************
			 ** Accessors **
			 ***************/

			Tlb * tlb() { return _tlb; }
	};


	/**
	 * Access to static interrupt-controller
	 */
	static Pic * pic() { static Pic _object; return &_object; }
}


bool Kernel::Irq_owner::allocate_irq(unsigned const irq)
{
	/* Check if an allocation is needed and possible */
	unsigned const id = irq_to_id(irq);
	if (_id) return _id == id;
	if (_pool()->object(id)) return 0;

	/* Let us own the IRQ, but mask it till we await it */
	pic()->mask(irq);
	_id = id;
	_pool()->insert(this);
	return 1;
}


bool Kernel::Irq_owner::free_irq(unsigned const irq)
{
	if (_id != irq_to_id(irq)) return 0;
	_pool()->remove(this);
	_id = 0;
	return 1;
}


void Kernel::Irq_owner::await_irq()
{
	assert(_id);
	unsigned const irq = id_to_irq(_id);
	pic()->unmask(irq);
	_awaits_irq();
}


void Kernel::Irq_owner::receive_irq(unsigned const irq)
{
	assert(_id == irq_to_id(irq));
	pic()->mask(irq);
	_received_irq();
}


Kernel::Irq_owner * Kernel::Irq_owner::owner(unsigned irq) {
	return _pool()->object(irq_to_id(irq)); }


namespace Kernel
{
	/**
	 * Idle thread entry
	 */
	static void idle_main() { while (1) ; }


	/**
	 * Access to static kernel timer
	 */
	static Timer * timer() { static Timer _object; return &_object; }


	/**
	 * Access to the static CPU scheduler
	 */
	static Cpu_scheduler * cpu_scheduler();


	/**
	 * Static kernel PD that describes core
	 */
	static Pd * core()
	{
		static Core_tlb tlb;
		static Pd _pd(&tlb);
		return &_pd;
	}


	/**
	 * Get core attributes
	 */
	unsigned core_id() { return core()->id(); }


	class Thread;

	void handle_pagefault(Thread * const);
	void handle_syscall(Thread * const);
	void handle_interrupt(void);
	void handle_invalid_excpt(void);
}


void Kernel::Thread::_activate()
{
	cpu_scheduler()->insert(this);
	_state = ACTIVE;
}


void Kernel::Thread::pause()
{
	assert(_state == AWAIT_RESUMPTION || _state == ACTIVE);
	cpu_scheduler()->remove(this);
	_state = AWAIT_RESUMPTION;
}


void Kernel::Thread::stop()
{
	cpu_scheduler()->remove(this);
	_state = STOPPED;
}


int Kernel::Thread::resume()
{
	if (_state != AWAIT_RESUMPTION && _state != ACTIVE) {
		PDBG("Unexpected thread state");
		return -1;
	}
	cpu_scheduler()->insert(this);
	if (_state == ACTIVE) return 1;
	_state = ACTIVE;
	return 0;
}


void Kernel::Thread::request_and_wait(Thread * const dest, size_t const size) {
	Ipc_node::send_request_await_reply(dest, phys_utcb()->base(),
	                                   size, phys_utcb()->base(),
	                                   phys_utcb()->size()); }


void Kernel::Thread::wait_for_request() {
	Ipc_node::await_request(phys_utcb()->base(),
	                        phys_utcb()->size()); }


void Kernel::Thread::reply(size_t const size, bool const await_request)
{
	Ipc_node::send_reply(phys_utcb()->base(), size);
	if (await_request)
		Ipc_node::await_request(phys_utcb()->base(),
		                        phys_utcb()->size());
	else user_arg_0(0);
}


void Kernel::Thread::await_signal()
{
	cpu_scheduler()->remove(this);
	_state = AWAIT_IRQ;
}


void Kernel::Thread::receive_signal(Signal const s)
{
	*(Signal *)phys_utcb()->base() = s;
	_activate();
}


void Kernel::Thread::handle_exception()
{
	switch(cpu_exception) {
	case SUPERVISOR_CALL:
		handle_syscall(this);
		return;
	case PREFETCH_ABORT:
	case DATA_ABORT:
		handle_pagefault(this);
		return;
	case INTERRUPT_REQUEST:
	case FAST_INTERRUPT_REQUEST:
		handle_interrupt();
		return;
	default:
		handle_invalid_excpt();
	}
}


void Kernel::Thread::scheduled_next()
{
	_mt_client_context_ptr = (addr_t)static_cast<Genode::Cpu_state*>(this);
	mtc()->virt_user_entry();
}


void Kernel::Thread::_has_received(size_t const s)
{
	user_arg_0(s);
	if (_state != ACTIVE) _activate();
}


void Kernel::Thread::_awaits_receipt()
{
	cpu_scheduler()->remove(this);
	_state = AWAIT_IPC;
}


void Kernel::Thread::_awaits_irq()
{
	cpu_scheduler()->remove(this);
	_state = AWAIT_IRQ;
}


namespace Kernel
{
	class Signal_receiver;

	/**
	 * Specific signal type, owned by a receiver, can be triggered asynchr.
	 */
	class Signal_context : public Object<Signal_context, MAX_SIGNAL_CONTEXTS>,
	                       public Fifo<Signal_context>::Element
	{
		friend class Signal_receiver;

		Signal_receiver * const _receiver; /* the receiver that owns us */
		unsigned const _imprint; /* every of our signals gets signed with */
		unsigned _number; /* how often we got triggered */

		public:

			/**
			 * Constructor
			 */
			Signal_context(Signal_receiver * const r,
			                 unsigned const imprint)
			: _receiver(r), _imprint(imprint), _number(0) { }

			/**
			 * Trigger this context
			 *
			 * \param number  how often this call triggers us at once
			 */
			void trigger_signal(unsigned const number);
	};

	/**
	 * Manage signal contexts and enable threads to trigger and await them
	 */
	class Signal_receiver :
		public Object<Signal_receiver, MAX_SIGNAL_RECEIVERS>
	{
		Fifo<Thread> _listeners;
		Fifo<Signal_context> _pending_contexts;

		/**
		 * Deliver as much submitted signals to listening threads as possible
		 */
		void _listen()
		{
			while (1)
			{
				/* any pending context? */
				if (_pending_contexts.empty()) return;
				Signal_context * const c = _pending_contexts.dequeue();

				/* if there is no listener, enqueue context again and return */
				if (_listeners.empty()) {
					_pending_contexts.enqueue(c);
					return;
				}
				/* awake a listener and transmit signal info to it */
				Thread * const t = _listeners.dequeue();
				t->receive_signal(Signal(c->_imprint, c->_number));

				/* reset context */
				c->_number = 0;
			}
		}

		public:

			/**
			 * Let a thread listen to our contexts
			 */
			void add_listener(Thread * const t)
			{
				t->await_signal();
				_listeners.enqueue(t);
				_listen();
			}

			/**
			 * If any of our contexts is pending
			 */
			bool pending() { return !_pending_contexts.empty(); }

			/**
			 * Recognize that one of our contexts was triggered
			 */
			void add_pending_context(Signal_context * const c)
			{
				assert(c->_receiver == this);
				if(!c->is_enqueued()) _pending_contexts.enqueue(c);
				_listen();
			}
	};


	class Vm : public Object<Vm, MAX_VMS>,
	           public Schedule_context
	{
		private:

			Genode::Cpu_state_modes * const _state;
			Signal_context * const          _context;

		public:

			void * operator new (size_t, void * p) { return p; }

			/**
			 * Constructor
			 */
			Vm(Genode::Cpu_state_modes * const state,
			   Signal_context * const context)
			: _state(state), _context(context) { }

			void run() {
				cpu_scheduler()->insert(this); }


			/**********************
			 ** Schedule_context **
			 **********************/

			void handle_exception()
			{
				switch(_state->cpu_exception) {
				case Genode::Cpu_state::INTERRUPT_REQUEST:
				case Genode::Cpu_state::FAST_INTERRUPT_REQUEST:
					handle_interrupt();
					return;
				default:
					cpu_scheduler()->remove(this);
					_context->trigger_signal(1);
				}
			}

			void scheduled_next()
			{
				/* set context pointer for mode switch */
				_mt_client_context_ptr = (addr_t)_state;

				/* jump to assembler path */
				((void(*)(void))&_mon_vm_entry)();
			}
	};


	/**
	 * Access to static CPU scheduler
	 */
	Cpu_scheduler * cpu_scheduler()
	{
		/* create idle thread */
		static char idle_stack[DEFAULT_STACK_SIZE]
		__attribute__((aligned(Cpu::DATA_ACCESS_ALIGNM)));
		static Thread idle((Platform_thread *)0);
		static bool initial = 1;
		if (initial)
		{
			/* initialize idle thread */
			void * sp;
			sp = (void *)&idle_stack[sizeof(idle_stack)/sizeof(idle_stack[0])];
			idle.init_context((void *)&idle_main, sp, core_id());
			initial = 0;
		}
		/* create scheduler with a permanent idle thread */
		static unsigned const user_time_slice =
		timer()->ms_to_tics(USER_TIME_SLICE_MS);
		static Cpu_scheduler cpu_sched(&idle, user_time_slice);
		return &cpu_sched;
	}

	/**
	 * Get attributes of the mode transition region in every PD
	 */
	addr_t mode_transition_virt_base() { return mtc()->VIRT_BASE; }
	size_t mode_transition_size()      { return mtc()->SIZE; }

	/**
	 * Get attributes of the kernel objects
	 */
	size_t thread_size()          { return sizeof(Thread); }
	size_t pd_size()              { return sizeof(Tlb) + sizeof(Pd); }
	size_t signal_context_size()  { return sizeof(Signal_context); }
	size_t signal_receiver_size() { return sizeof(Signal_receiver); }
	unsigned pd_alignm_log2()     { return Tlb::ALIGNM_LOG2; }
	size_t vm_size()              { return sizeof(Vm); }


	/**
	 * Handle the occurence of an unknown exception
	 */
	void handle_invalid_excpt() { assert(0); }


	/**
	 * Handle an interrupt request
	 */
	void handle_interrupt()
	{
		/* determine handling for specific interrupt */
		unsigned irq;
		if (pic()->take_request(irq))
		{
			switch (irq) {

			case Timer::IRQ: {

				/* clear interrupt at timer */
				timer()->clear_interrupt();
				break; }

			default: {

				/* IRQ not owned by core, thus notify IRQ owner */
				Irq_owner * const o = Irq_owner::owner(irq);
				assert(o);
				o->receive_irq(irq);
				break; }
			}
		}
		/* disengage interrupt controller from IRQ */
		pic()->finish_request();
	}


	/**
	 * Handle an usermode pagefault
	 *
	 * \param user  thread that has caused the pagefault
	 */
	void handle_pagefault(Thread * const user)
	{
		/* check out cause and attributes of abort */
		addr_t va;
		bool w;
		assert(user->translation_miss(va, w));

		/* the user might be able to resolve the pagefault */
		user->pagefault(va, w);
	}


	/**
	 * Handle request of an unknown signal type
	 */
	void handle_invalid_syscall(Thread * const) { assert(0); }


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_new_pd(Thread * const user)
	{
		/* check permissions */
		assert(user->pd_id() == core_id());

		/* create TLB and PD */
		void * dst = (void *)user->user_arg_1();
		Tlb * const tlb = new (dst) Tlb();
		dst = (void *)((addr_t)dst + sizeof(Tlb));
		Pd * const pd = new (dst) Pd(tlb);

		/* return success */
		user->user_arg_0(pd->id());
	}


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_new_thread(Thread * const user)
	{
		/* check permissions */
		assert(user->pd_id() == core_id());

		/* dispatch arguments */
		Syscall_arg const arg1 = user->user_arg_1();
		Syscall_arg const arg2 = user->user_arg_2();

		/* create thread */
		Thread * const t = new ((void *)arg1)
		Thread((Platform_thread *)arg2);

		/* return thread ID */
		user->user_arg_0((Syscall_ret)t->id());
	}

	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_delete_thread(Thread * const user)
	{
		/* check permissions */
		assert(user->pd_id() == core_id());

		/* get targeted thread */
		unsigned thread_id = (unsigned)user->user_arg_1();
		Thread * const thread = Thread::pool()->object(thread_id);
		assert(thread);

		/* destroy thread */
		thread->~Thread();
	}


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_start_thread(Thread * const user)
	{
		/* check permissions */
		assert(user->pd_id() == core_id());

		/* dispatch arguments */
		Platform_thread * pt = (Platform_thread *)user->user_arg_1();
		void * const ip = (void *)user->user_arg_2();
		void * const sp = (void *)user->user_arg_3();
		unsigned const cpu = (unsigned)user->user_arg_4();

		/* get targeted thread */
		Thread * const t = Thread::pool()->object(pt->id());
		assert(t);

		/* start thread */
		assert(!t->start(ip, sp, cpu, pt->pd_id(),
		                 pt->phys_utcb(), pt->virt_utcb()))

		/* return software TLB that the thread is assigned to */
		Pd::Pool * const pp = Pd::pool();
		Pd * const pd = pp->object(t->pd_id());
		assert(pd);
		user->user_arg_0((Syscall_ret)pd->tlb());
	}


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_pause_thread(Thread * const user)
	{
		unsigned const tid = user->user_arg_1();

		/* shortcut for a thread to pause itself */
		if (!tid) {
			user->pause();
			user->user_arg_0(0);
			return;
		}

		/* get targeted thread and check permissions */
		Thread * const t = Thread::pool()->object(tid);
		assert(t && (user->pd_id() == core_id() || user==t));

		/* pause targeted thread */
		t->pause();
		user->user_arg_0(0);
	}


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_resume_thread(Thread * const user)
	{
		/* get targeted thread */
		Thread * const t = Thread::pool()->object(user->user_arg_1());
		assert(t);

		/* check permissions */
		assert(user->pd_id() == core_id() || user->pd_id() == t->pd_id());

		/* resume targeted thread */
		user->user_arg_0(t->resume());
	}


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_resume_faulter(Thread * const user)
	{
		/* get targeted thread */
		Thread * const t = Thread::pool()->object(user->user_arg_1());
		assert(t);

		/* check permissions */
		assert(user->pd_id() == core_id() || user->pd_id() == t->pd_id());

		/*
		 * Writeback the TLB entry that resolves the fault.
		 * This is a substitution for write-through-flagging
		 * the memory that holds the TLB data, because the latter
		 * is not feasible in core space.
		 */
		Cpu::tlb_insertions();

		/* resume targeted thread */
		t->resume();
	}


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_yield_thread(Thread * const user)
	{
		/* get targeted thread */
		Thread * const t = Thread::pool()->object(user->user_arg_1());

		/* invoke kernel object */
		if (t) t->resume();
		cpu_scheduler()->yield();
	}


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_current_thread_id(Thread * const user)
	{ user->user_arg_0((Syscall_ret)user->id()); }


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_get_thread(Thread * const user)
	{
		/* check permissions */
		assert(user->pd_id() == core_id());

		/* get target */
		unsigned const tid = (unsigned)user->user_arg_1();
		Thread * t;

		/* user targets a thread by ID */
		if (tid) {
			t = Thread::pool()->object(tid);
			assert(t);

		/* user targets itself */
		} else t = user;

		/* return target platform thread */
		user->user_arg_0((Syscall_ret)t->platform_thread());
	}


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_wait_for_request(Thread * const user)
	{
		user->wait_for_request();
	}


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_request_and_wait(Thread * const user)
	{
		/* get IPC receiver */
		Thread * const t = Thread::pool()->object(user->user_arg_1());
		assert(t);

		/* do IPC */
		user->request_and_wait(t, (size_t)user->user_arg_2());
	}


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_reply(Thread * const user) {
		user->reply((size_t)user->user_arg_1(), (bool)user->user_arg_2()); }


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_set_pager(Thread * const user)
	{
		/* assert preconditions */
		assert(user->pd_id() == core_id());

		/* get faulter and pager thread */
		Thread * const p = Thread::pool()->object(user->user_arg_1());
		Thread * const f = Thread::pool()->object(user->user_arg_2());
		assert(p && f);

		/* assign pager */
		f->pager(p);
	}


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_update_pd(Thread * const user)
	{
		assert(user->pd_id() == core_id());
		Cpu::flush_tlb_by_pid(user->user_arg_1());
	}


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_allocate_irq(Thread * const user)
	{
		assert(user->pd_id() == core_id());
		unsigned irq = user->user_arg_1();
		user->user_arg_0(user->allocate_irq(irq));
	}


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_free_irq(Thread * const user)
	{
		assert(user->pd_id() == core_id());
		unsigned irq = user->user_arg_1();
		user->user_arg_0(user->free_irq(irq));
	}


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_await_irq(Thread * const user)
	{
		assert(user->pd_id() == core_id());
		user->await_irq();
	}


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_print_char(Thread * const user)
	{
		Genode::printf("%c", (char)user->user_arg_1());
	}


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_read_thread_state(Thread * const user)
	{
		assert(user->pd_id() == core_id());
		Thread * const t = Thread::pool()->object(user->user_arg_1());
		if (!t) PDBG("Targeted thread unknown");
		Thread_state * const ts = (Thread_state *)user->phys_utcb()->base();
		t->Cpu::Context::read_cpu_state(ts);
	}


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_write_thread_state(Thread * const user)
	{
		assert(user->pd_id() == core_id());
		Thread * const t = Thread::pool()->object(user->user_arg_1());
		if (!t) PDBG("Targeted thread unknown");
		Thread_state * const ts = (Thread_state *)user->phys_utcb()->base();
		t->Cpu::Context::write_cpu_state(ts);
	}


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_new_signal_receiver(Thread * const user)
	{
			/* check permissions */
			assert(user->pd_id() == core_id());

			/* create receiver */
			void * dst = (void *)user->user_arg_1();
			Signal_receiver * const r = new (dst) Signal_receiver();

			/* return success */
			user->user_arg_0(r->id());
	}


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_new_signal_context(Thread * const user)
	{
		/* check permissions */
		assert(user->pd_id() == core_id());

		/* lookup receiver */
		unsigned rid = user->user_arg_2();
		Signal_receiver * const r = Signal_receiver::pool()->object(rid);
		assert(r);

		/* create context */
		void * dst = (void *)user->user_arg_1();
		unsigned imprint = user->user_arg_3();
		Signal_context * const c = new (dst) Signal_context(r, imprint);

		/* return success */
		user->user_arg_0(c->id());
	}


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_await_signal(Thread * const user)
	{
		/* lookup receiver */
		unsigned rid = user->user_arg_2();
		Signal_receiver * const r = Signal_receiver::pool()->object(rid);
		assert(r);

		/* let user listen to receiver */
		r->add_listener(user);
	}


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_signal_pending(Thread * const user)
	{
		/* lookup receiver */
		unsigned rid = user->user_arg_2();
		Signal_receiver * const r = Signal_receiver::pool()->object(rid);
		assert(r);

		/* set return value */
		user->user_arg_0(r->pending());
	}


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_submit_signal(Thread * const user)
	{
		/* lookup context */
		Signal_context * const c =
			Signal_context::pool()->object(user->user_arg_1());
		assert(c);

		/* trigger signal at context */
		c->trigger_signal(user->user_arg_2());
	}


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_new_vm(Thread * const user)
	{
		/* check permissions */
		assert(user->pd_id() == core_id());

		/* dispatch arguments */
		void * const allocator = (void * const)user->user_arg_1();
		Genode::Cpu_state_modes * const state =
			(Genode::Cpu_state_modes * const)user->user_arg_2();
		Signal_context * const context =
			Signal_context::pool()->object(user->user_arg_3());
		assert(context);

		/* create vm */
		Vm * const vm = new (allocator) Vm(state, context);

		/* return vm id */
		user->user_arg_0((Syscall_ret)vm->id());
	}


	/**
	 * Do specific syscall for 'user', for details see 'syscall.h'
	 */
	void do_run_vm(Thread * const user)
	{
		/* check permissions */
		assert(user->pd_id() == core_id());

		/* get targeted vm via its id */
		Vm * const vm = Vm::pool()->object(user->user_arg_1());
		assert(vm);

		/* run targeted vm */
		vm->run();
	}


	/**
	 * Handle a syscall request
	 *
	 * \param user  thread that called the syscall
	 */
	void handle_syscall(Thread * const user)
	{
		/* map syscall types to the according handler functions */
		typedef void (*Syscall_handler)(Thread * const);
		static Syscall_handler const handle_sysc[] =
		{
			/* syscall ID */ /* handler           */
			/*------------*/ /*-------------------*/
			/*  0         */ handle_invalid_syscall,
			/*  1         */ do_new_thread,
			/*  2         */ do_start_thread,
			/*  3         */ do_pause_thread,
			/*  4         */ do_resume_thread,
			/*  5         */ do_get_thread,
			/*  6         */ do_current_thread_id,
			/*  7         */ do_yield_thread,
			/*  8         */ do_request_and_wait,
			/*  9         */ do_reply,
			/* 10         */ do_wait_for_request,
			/* 11         */ do_set_pager,
			/* 12         */ do_update_pd,
			/* 13         */ do_new_pd,
			/* 14         */ do_allocate_irq,
			/* 15         */ do_await_irq,
			/* 16         */ do_free_irq,
			/* 17         */ do_print_char,
			/* 18         */ do_read_thread_state,
			/* 19         */ do_write_thread_state,
			/* 20         */ do_new_signal_receiver,
			/* 21         */ do_new_signal_context,
			/* 22         */ do_await_signal,
			/* 23         */ do_submit_signal,
			/* 24         */ do_new_vm,
			/* 25         */ do_run_vm,
			/* 26         */ do_delete_thread,
			/* 27         */ do_signal_pending,
			/* 28         */ do_resume_faulter,
		};
		enum { MAX_SYSCALL = sizeof(handle_sysc)/sizeof(handle_sysc[0]) - 1 };

		/* handle syscall that has been requested by the user */
		unsigned syscall = user->user_arg_0();
		if (syscall > MAX_SYSCALL) handle_sysc[INVALID_SYSCALL](user);
		else handle_sysc[syscall](user);
	}
}

/**
 * Prepare the system for the first run of 'kernel'
 */
extern "C" void init_phys_kernel() {
	Cpu::init_phys_kernel(); }

/**
 * Kernel main routine
 */
extern "C" void kernel()
{
	static unsigned user_time = 0;
	static bool initial_call = true;

	/* an exception occured */
	if (!initial_call)
	{
		/* update how much time the last user has consumed */
		unsigned const timer_value = timer()->stop_one_shot();
		user_time = timer_value < user_time ? user_time - timer_value : 0;

		/* handle exception that interrupted the last user */
		cpu_scheduler()->current_entry()->handle_exception();

	/* kernel initialization */
	} else {

		/* compose kernel CPU context */
		static Cpu::Context kernel_context;
		kernel_context.ip = (addr_t)kernel;
		kernel_context.sp = (addr_t)&_kernel_stack_high;

		/* add kernel to the core PD */
		core()->append_context(&kernel_context);

		/* offer the final kernel context to the mode transition page */
		mtc()->fetch_master_context(&kernel_context);

		/* TrustZone initialization code */
		trustzone_initialization(pic());

		/* switch to core address space */
		Cpu::init_virt_kernel(core()->tlb()->base(), core_id());

		/* create the core main thread */
		static Native_utcb cm_utcb;
		static char cm_stack[DEFAULT_STACK_SIZE]
		            __attribute__((aligned(Cpu::DATA_ACCESS_ALIGNM)));
		static Thread core_main((Platform_thread *)0);
		_main_utcb = &cm_utcb;
		enum { CM_STACK_SIZE = sizeof(cm_stack)/sizeof(cm_stack[0]) + 1 };
		core_main.start((void *)CORE_MAIN,
		                (void *)&cm_stack[CM_STACK_SIZE - 1],
		                0, core_id(), &cm_utcb, &cm_utcb);

		/* kernel initialization finished */
		initial_call = false;
	}
	/* offer next user context to the mode transition PIC */
	Schedule_context * const next = cpu_scheduler()->next_entry(user_time);

	/* limit user mode execution in time */
	timer()->start_one_shot(user_time);
	pic()->unmask(Timer::IRQ);

	/* will jump to the context related mode-switch */
	next->scheduled_next();
}


/********************
 ** Kernel::Thread **
 ********************/

int Thread::start(void *ip, void *sp, unsigned cpu_no,
                         unsigned const pd_id,
                         Native_utcb * const phys_utcb,
                         Native_utcb * const virt_utcb)
{
	/* check state and arguments */
	assert(_state == STOPPED)
	assert(!cpu_no);

	/* apply thread configuration */
	init_context(ip, sp, pd_id);
	_phys_utcb = phys_utcb;
	_virt_utcb = virt_utcb;

	/* offer thread-entry arguments */
	user_arg_0((unsigned)_virt_utcb);

	/* start thread */
	cpu_scheduler()->insert(this);
	_state = ACTIVE;
	return 0;
}


void Thread::init_context(void * const instr_p, void * const stack_p,
                                  unsigned const pd_id)
{
	/* basic thread state */
	sp = (addr_t)stack_p;
	ip = (addr_t)instr_p;

	/* join a pd */
	_pd_id = pd_id;
	Pd * const pd = Pd::pool()->object(_pd_id);
	assert(pd)
	protection_domain(pd_id);
	tlb(pd->tlb()->base());
}


void Thread::pagefault(addr_t const va, bool const w)
{
	/* pause faulter */
	cpu_scheduler()->remove(this);
	_state = AWAIT_RESUMPTION;

	/* inform pager through IPC */
	assert(_pager);
	_pagefault = Pagefault(id(), (Tlb *)tlb(), ip, va, w);
	Ipc_node::send_note(_pager, &_pagefault, sizeof(_pagefault));
}


/****************************
 ** Kernel::Signal_context **
 ****************************/


void Signal_context::trigger_signal(unsigned const number)
{
	/* raise our number */
	unsigned const old_nr = _number;
	_number += number;
	assert(old_nr <= _number);

	/* notify our receiver */
	if (_number) _receiver->add_pending_context(this);
}


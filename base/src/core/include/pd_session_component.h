/*
 * \brief  Core-specific instance of the PD session interface
 * \author Christian Helmuth
 * \date   2006-07-17
 */

/*
 * Copyright (C) 2006-2013 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _CORE__INCLUDE__PD_SESSION_COMPONENT_H_
#define _CORE__INCLUDE__PD_SESSION_COMPONENT_H_

/* Genode includes */
#include <base/rpc_server.h>
#include <pd_session/pd_session.h>

/* core includes */
#include <platform_pd.h>

namespace Genode {

	class Pd_session_component : public Rpc_object<Pd_session>
	{
		private:

			Platform_pd        _pd;
			Parent_capability  _parent;
			Rpc_entrypoint    *_thread_ep;

		public:

			Pd_session_component(Rpc_entrypoint *thread_ep, const char *args)
			: _thread_ep(thread_ep) { }


			/**************************/
			/** PD session interface **/
			/**************************/

			int bind_thread(Thread_capability);
			int assign_parent(Parent_capability);
	};
}

#endif /* _CORE__INCLUDE__PD_SESSION_COMPONENT_H_ */

//! \file
/*
**  Copyright (c) 2016 - Ponce
**  Authors:
**         Alberto Garcia Illera        agarciaillera@gmail.com
**         Francisco Oca                francisco.oca.gonzalez@gmail.com
**
**  This program is under the terms of the BSD License.
*/

#include <list>
// Ponce
#include "callbacks.hpp"
#include "globals.hpp"
#include "context.hpp"
#include "utils.hpp"
#include "tainting_n_symbolic.hpp"
#include "blacklist.hpp"
#include "actions.hpp"

//IDA
#include <ida.hpp>
#include <dbg.hpp>
#include <loader.hpp>
#include <intel.hpp>
#include <bytes.hpp>
//Triton
#include "triton/api.hpp"
#include "triton/x86Specifications.hpp"

std::list<breakpoint_pending_action> breakpoint_pending_actions;

/*This function will create and fill the Triton object for every instruction
	Returns:
	0 instruction tritonized
	1 trigger is not activated
	2 other error*/
int tritonize(ea_t pc, thid_t threadID)
{
	/*Check that the runtime Trigger is on just in case*/
	if (!ponce_runtime_status.runtimeTrigger.getState())
		return 1;

	threadID = threadID ? threadID : get_current_thread();

	if (pc == 0) {
		msg("[!] Some error at tritonize since pc is 0");
		return 2;
	}

	//We delete the last_instruction
	if (ponce_runtime_status.last_triton_instruction != nullptr){
		delete ponce_runtime_status.last_triton_instruction;
		ponce_runtime_status.last_triton_instruction = nullptr;
	}

	triton::arch::Instruction* tritonInst = new triton::arch::Instruction();
	ponce_runtime_status.last_triton_instruction = tritonInst;

	/*This will fill the 'cmd' (to get the instruction size) which is a insn_t structure https://www.hex-rays.com/products/ida/support/sdkdoc/classinsn__t.html */
	if (!can_decode(pc)) {
		msg("[!] Some error decoding instruction at " MEM_FORMAT, pc);
	}
	
	unsigned char opcodes[15];
	ssize_t item_size = 0x0;

	insn_t ins;
	decode_insn(&ins, pc);
	item_size = ins.size;
	assert(item_size < sizeof(opcodes));
	get_bytes(&opcodes, item_size, pc, GMB_READALL, NULL);

	/* Setup Triton information */
	tritonInst->clear(); // ToDo: I think this is not necesary
	tritonInst->setOpcode((triton::uint8*)opcodes, item_size);
	tritonInst->setAddress(pc);
	tritonInst->setThreadId(threadID);

	try {
		if (!api.processing(*tritonInst)) {
			msg("[!] Instruction at " MEM_FORMAT " not supported by Triton: %s (Thread id: %d)\n", pc, tritonInst->getDisassembly().c_str(), threadID);
			return 2;
		}
	}
	catch (const triton::exceptions::Exception& e) {
		msg("[!] Instruction at " MEM_FORMAT " not supported by Triton: %s (Thread id: %d)\n", pc, tritonInst->getDisassembly().c_str(), threadID);
		return 2;
	}

	if (cmdOptions.showExtraDebugInfo) {
		msg("[+] Triton at " MEM_FORMAT " : %s (Thread id: %d)\n", pc, tritonInst->getDisassembly().c_str(), threadID);
	}

	/*In the case that the snapshot engine is in use we should track every memory write access*/
	if (snapshot.exists())
	{
		auto store_access_list = tritonInst->getStoreAccess();
		for (auto it = store_access_list.begin(); it != store_access_list.end(); it++)
		{
			triton::arch::MemoryAccess memory_access = it->first;
			auto addr = memory_access.getAddress();
			//This is the way to force IDA to read the value from the debugger
			//More info here: https://www.hex-rays.com/products/ida/support/sdkdoc/dbg_8hpp.html#ac67a564945a2c1721691aa2f657a908c
			invalidate_dbgmem_contents((ea_t)addr, memory_access.getSize()); //ToDo: Do I have to call this for every byte in memory I want to read?
			for (unsigned int i = 0; i < memory_access.getSize(); i++)
			{
				triton::uint128 value = 0;
				//We get the memory readed
				get_bytes(&value, 1, (ea_t)addr+i, GMB_READALL, NULL);

				//We add a meomory modification to the snapshot engine
				snapshot.addModification((ea_t)addr + i, value.convert_to<char>());
			}
		}
	}

	if (cmdOptions.addCommentsControlledOperands)
		get_controlled_operands_and_add_comment(tritonInst, pc);

	if (cmdOptions.addCommentsSymbolicExpresions)
		add_symbolic_expressions(tritonInst, pc);

	if (cmdOptions.paintExecutedInstructions)
	{
		//We only paint the executed instructions if they don't have a previous color
		if (get_item_color(pc) == 0xffffffff)
			set_item_color(pc, cmdOptions.color_executed_instruction);
	}

	//ToDo: The isSymbolized is missidentifying like "user-controlled" some instructions: https://github.com/JonathanSalwan/Triton/issues/383
	if (tritonInst->isTainted() || tritonInst->isSymbolized())
	{
		ponce_runtime_status.total_number_symbolic_ins++;

		if (cmdOptions.showDebugInfo) {
			msg("[!] Instruction %s at " MEM_FORMAT " \n", tritonInst->isTainted() ? "tainted" : "symbolized", pc);
		}
		if (cmdOptions.RenameTaintedFunctionNames)
			rename_tainted_function(pc);
		// Check if it is a conditional jump
		// We only color with a different color the symbolic conditions, to show the user he could do additional actions like solve
		if (tritonInst->isBranch())
		{
			ponce_runtime_status.total_number_symbolic_conditions++;
			if (cmdOptions.use_symbolic_engine)
				set_item_color(pc, cmdOptions.color_tainted_condition);
			else
				set_item_color(pc, cmdOptions.color_tainted);
		}
	}

	if (tritonInst->isBranch() && tritonInst->isSymbolized())
	{
		ea_t addr1 = (ea_t)tritonInst->getNextAddress();
		ea_t addr2 = (ea_t)tritonInst->operands[0].getImmediate().getValue();
		if (cmdOptions.showDebugInfo) {
			msg("[+] Branch symbolized detected at " MEM_FORMAT ": " MEM_FORMAT " or " MEM_FORMAT ", Taken:%s\n", pc, addr1, addr2, tritonInst->isConditionTaken() ? "Yes" : "No");
		}
		triton::usize ripId = 0;
		ripId = api.getSymbolicRegister(REG_XIP)->getId();

		if (tritonInst->isConditionTaken())
			ponce_runtime_status.myPathConstraints.push_back(PathConstraint(ripId, pc, addr2, addr1, ponce_runtime_status.myPathConstraints.size()));
		else
			ponce_runtime_status.myPathConstraints.push_back(PathConstraint(ripId, pc, addr1, addr2, ponce_runtime_status.myPathConstraints.size()));
	}
	return 0;
	//We add the instruction to the map, so we can use it later to negate conditions, view SE, slicing, etc..
	//instructions_executed_map[pc].push_back(tritonInst);
}

/*This functions is called every time a new debugger session starts*/
void triton_restart_engines()
{
	if (cmdOptions.showDebugInfo)
		msg("[+] Restarting triton engines...\n");
	//We need to set the architecture for Triton
	if (inf_is_64bit())
		api.setArchitecture(triton::arch::ARCH_X86_64);
	else
		api.setArchitecture(triton::arch::ARCH_X86);
	//We reset everything at the beginning
	api.reset();
	// Memory access callback
	api.addCallback(needConcreteMemoryValue_cb);
	// Register access callback
	api.addCallback(needConcreteRegisterValue_cb);
	//If we are in taint analysis mode we enable only the tainting engine and disable the symbolic one
	api.getTaintEngine()->enable(cmdOptions.use_tainting_engine);
	api.getSymbolicEngine()->enable(cmdOptions.use_symbolic_engine);
	if (ponce_runtime_status.last_triton_instruction) {
		delete ponce_runtime_status.last_triton_instruction;
		ponce_runtime_status.last_triton_instruction = nullptr;
	}
	// This optimization is veeery good for the size of the formulas
	//api.enableSymbolicOptimization(triton::engines::symbolic:: ALIGNED_MEMORY, true);
	//api.setMode(triton::modes::ALIGNED_MEMORY, true);

	// We only are symbolic or taint executing an instruction if it is tainted, so it is a bit faster and we save a lot of memory
	//if (cmdOptions.only_on_optimization)
	//{
	//	if (cmdOptions.use_symbolic_engine)
	//	{
	//		api.setMode(triton::modes::ONLY_ON_SYMBOLIZED, true);
	//		/*api.enableSymbolicOptimization(triton::engines::symbolic::AST_DICTIONARIES, true); // seems not to exist any more
	//		api.enableSymbolicOptimization(triton::engines::symbolic::ONLY_ON_SYMBOLIZED, true);*/
	//	}
	//	if (cmdOptions.use_tainting_engine)
	//	{
	//		//We need to disable this optimization using the taint engine, if not a lot of RAM is consumed
	//		api.setMode(triton::modes::ONLY_ON_SYMBOLIZED, true); 
	//		/*api.enableSymbolicOptimization(triton::engines::symbolic::AST_DICTIONARIES, false);
	//		api.enableSymbolicOptimization(triton::engines::symbolic::ONLY_ON_TAINTED, true);*/
	//	}
	//}
	//triton::api.getSymbolicEngine()->enable(true);
	ponce_runtime_status.runtimeTrigger.disable();
	ponce_runtime_status.is_something_tainted_or_symbolize = false;
	ponce_runtime_status.tainted_functions_index = 0;
	//Reset instruction counter
	ponce_runtime_status.total_number_traced_ins = 0;
	ponce_runtime_status.total_number_symbolic_ins = 0;
	ponce_runtime_status.total_number_symbolic_conditions  = 0;
	ponce_runtime_status.current_trace_counter = 0;
	breakpoint_pending_actions.clear();
	set_automatic_taint_n_simbolic();
	ponce_runtime_status.myPathConstraints.clear();
}


ssize_t idaapi tracer_callback(void *user_data, int notification_code, va_list va)
{
	if (cmdOptions.showExtraDebugInfo)
		msg("[+] Notification code: %d str: %s\n",notification_code, notification_code_to_string(notification_code).c_str());
	switch (notification_code)
	{
		case dbg_process_start:
		case dbg_process_attach:
		{
			if (cmdOptions.showDebugInfo)
				msg("[+] Starting the debugged process. Reseting all the engines.\n");
			triton_restart_engines();
			clear_requests_queue();
			break;
		}
		case dbg_step_into:
		case dbg_step_over:
		{
			ponce_runtime_status.tracing_start_time = 0;
			break;
		}
		case dbg_trace:
		{
			//We only want to analyze the thread being analyzed
			if (ponce_runtime_status.analyzed_thread != get_current_thread())
				break;
			//If the trigger is disbaled then the user is manually stepping with the ponce tracing disabled
			if (!ponce_runtime_status.runtimeTrigger.getState())
				break;

			thid_t tid = va_arg(va, thid_t);
			ea_t pc = va_arg(va, ea_t);
			//Sometimes the cmd structure doesn't correspond with the traced instruction
			//With this we are filling cmd with the instruction at the address specified

			if (should_blacklist(pc, tid)) {
				// We have blacklisted this call so we should not keep going 
				return 0;
			}
			
			//If the instruciton is not a blacklisted call we analyze the instruction
			//We don't want to reanalize instructions. p.e. if we put a bp we receive two events, the bp and this one
			if (ponce_runtime_status.last_triton_instruction == NULL || (ponce_runtime_status.last_triton_instruction != NULL && ponce_runtime_status.last_triton_instruction->getAddress() != pc))
				tritonize(pc, tid);

			ponce_runtime_status.current_trace_counter++;
			ponce_runtime_status.total_number_traced_ins++;
			//Every 1000 traced instructions we show with debug that info in the output
			if (cmdOptions.showDebugInfo && ponce_runtime_status.total_number_traced_ins % 1000 == 0)
				msg("Instructions traced: %d Symbolic instructions: %d Symbolic conditions: %d Time: %lld secs\n", ponce_runtime_status.total_number_traced_ins, ponce_runtime_status.total_number_symbolic_ins, ponce_runtime_status.total_number_symbolic_conditions, GetTimeMs64() - ponce_runtime_status.tracing_start_time);
				//msg("[+] Instructions traced: %d\n", ponce_runtime_status.total_number_traced_ins);

			//This is the wow64 switching, we need to skip it. https://forum.hex-rays.com/viewtopic.php?f=8&t=4070
			if (ponce_runtime_status.last_triton_instruction->getDisassembly().find("call dword ptr fs:[0xc0]") != -1)
			{
				if (cmdOptions.showExtraDebugInfo)
					msg("[+] Wow64 switching! Requesting a step_over\n");
				//And now we need to stop the tracing, do step over and reenable the tracing...
				//disable_step_trace();
				suspend_process();
				//We don't want to do a real step over (it would reset the timer)
				ponce_runtime_status.ignore_wow64_switching_step = true;
				request_step_over();
				request_continue_process();
				run_requests();
				break;
			}

			//Check if the limit instructions limit was reached
			if (cmdOptions.limitInstructionsTracingMode && ponce_runtime_status.current_trace_counter >= cmdOptions.limitInstructionsTracingMode)
			{
				int answer = ask_yn(1, "[?] %u instructions has been traced. Do you want to execute %u more?", ponce_runtime_status.total_number_traced_ins, (unsigned int)cmdOptions.limitInstructionsTracingMode);
				if (answer == 0 || answer == -1) //No or Cancel
				{
					// stop the trace mode and suspend the process
					disable_step_trace();
					suspend_process();
					msg("[!] Process suspended (Traced %d instructions)\n", ponce_runtime_status.total_number_traced_ins);
				}
				else
				{
					ponce_runtime_status.current_trace_counter = 0;
				}
			}
			
			//Check if the time limit for tracing was reached
			if (cmdOptions.limitTime != 0)
			{
				//This is the first time we start the tracer
				if (ponce_runtime_status.tracing_start_time == 0)
				{
					ponce_runtime_status.tracing_start_time = GetTimeMs64();
				}
				else if ((GetTimeMs64() - ponce_runtime_status.tracing_start_time) / 1000 >= cmdOptions.limitTime)
				{
					int answer = ask_yn(1, "[?] the tracing was working for %u seconds(%u inst traced!). Do you want to execute it %u more?", (unsigned int)((GetTimeMs64() - ponce_runtime_status.tracing_start_time) / 1000), ponce_runtime_status.total_number_traced_ins, (unsigned int)cmdOptions.limitTime);
					if (answer == 0 || answer == -1) //No or Cancel
					{
						// stop the trace mode and suspend the process
						disable_step_trace();
						suspend_process();
						msg("[!] Process suspended (Traced %d instructions)\n", ponce_runtime_status.total_number_traced_ins);
					}
					else
					{
						ponce_runtime_status.tracing_start_time = GetTimeMs64();
					}
				}
			}
			break;
		}
		case dbg_bpt:
		{
			//We only want to analyze the thread being analyzed
			if (ponce_runtime_status.analyzed_thread != get_current_thread())
				break;
			msg("BP Instructions traced: %d Symbolic instructions: %d Symbolic conditions: %d Time: %lld secs\n", ponce_runtime_status.total_number_traced_ins, ponce_runtime_status.total_number_symbolic_ins, ponce_runtime_status.total_number_symbolic_conditions, GetTimeMs64() - ponce_runtime_status.tracing_start_time);

			thid_t tid = va_arg(va, thid_t);
			ea_t pc = va_arg(va, ea_t);
			int *warn = va_arg(va, int *);
			//This variable defines if a breakpoint is a user-defined breakpoint or not
			bool user_bp = true;
			//We look if there is a pending action for this breakpoint
			for (auto it = breakpoint_pending_actions.begin(); it != breakpoint_pending_actions.end(); ++it)
			{
				breakpoint_pending_action bpa = *it;
				//If we find a pendign action we execute the callback
				if (pc == bpa.address)
				{
					bpa.callback(pc);
					tritonize(pc, tid);
					
					//The pending breakpoints are used for enable the tracing so we consider this instruction tracing too
					ponce_runtime_status.current_trace_counter++;
					ponce_runtime_status.total_number_traced_ins++;
					//If there is a user-defined bp in the same address we should respect it and dont continue the exec
					if (!bpa.ignore_breakpoint)
					{
						//If it's a breakpoint the plugin set not a user-defined bp
						user_bp = false;
						//If not this is the bp we set to taint the arguments, we should rmeove it and continue the execution
						del_bpt(pc);
						enable_step_trace(true);
						//We dont want to skip library funcions or debug segments
						set_step_trace_options(0);
						continue_process();
						//We delete the comment
						set_cmt(pc, "", false);
						breakpoint_pending_actions.erase(it);
					}
					break;
				}
			}
			//If it is a user break point we enable again the step tracing if it was enabled previously...
			//The idea is if the user uses Execute native til next bp, and IDA reachs the next bp we reenable the tracing
			if (user_bp)
			{
				ponce_runtime_status.tracing_start_time = 0;
				//request_suspend_process();
				//run_requests();
				//disable_step_trace();
				//request_enable_step_trace();
				//If the trigger is disabled then the user is manually stepping with the ponce tracing disabled
				//if (ponce_runtime_status.runtimeTrigger.getState())
				//enable_step_trace(ponce_runtime_status.runtimeTrigger.getState());
			}
			break;
		}
		case dbg_process_exit:
		{
			if (cmdOptions.showDebugInfo)
				msg("[!] Process_exiting...\n");
			//Do we want to unhook this event? I don't think so we want to be hooked for future sessions
			//unhook_from_notification_point(HT_DBG, tracer_callback, NULL);
			ponce_runtime_status.runtimeTrigger.disable();
			//Removing snapshot if it exists
			if (snapshot.exists())
				snapshot.resetEngine();
			break;
		}
	}
	return 0;
}

//---------------------------------------------------------------------------
// Callback for ui notifications
ssize_t idaapi ui_callback(void* ud, int notification_code, va_list va)
{
	switch (notification_code)
	{
		// Called when IDA is preparing a context menu for a view
		// Here dynamic context-depending user menu items can be added.
		case ui_populating_widget_popup:
		{
			TWidget *form = va_arg(va, TWidget *);
			TPopupMenu *popup_handle = va_arg(va, TPopupMenu *);
			int view_type = get_widget_type(form);

			//Adding a separator
			attach_action_to_popup(form, popup_handle, "", SETMENU_INS);

			/*Iterate over all the actions*/			
			for (int i = 0;; i++)
			{
				if (action_list[i].action_decs == NULL)
					break;
			
				/*Iterate over the view types of every action*/
				for (int j=0;; j++)
				{
					if (action_list[i].view_type[j] == __END__){
						break;
					}
					if (action_list[i].view_type[j] == view_type)
					{
						//We only attach to the popup if the action makes sense with the current configuration
						if (cmdOptions.use_tainting_engine && action_list[i].enable_taint || cmdOptions.use_symbolic_engine && action_list[i].enable_symbolic)
						{
							attach_action_to_popup(form, popup_handle, action_list[i].action_decs->name, action_list[i].menu_path, SETMENU_INS);
						}
					}
				}	
			}

			//Adding a separator
			attach_action_to_popup(form, popup_handle, "", SETMENU_INS);
			break;
		}
		case ui_finish_populating_widget_popup:
		{
			//This event is call after all the Ponce menus have been added and updated
			//It is the perfect point to add the multiple condition solve submenus
			TWidget *form = va_arg(va, TWidget *);
			TPopupMenu *popup_handle = va_arg(va, TPopupMenu *);
			int view_type = get_widget_type(form);

			//We get the ea form a global variable that is set in the update event
			//This is not very elegant but I don't know how to do it from here
			ea_t cur_ea = popup_menu_ea;
			if (view_type == BWN_DISASM)
			{
				//Adding submenus for solve with all the conditions executed in the same address
				for (unsigned int i = 0; i < ponce_runtime_status.myPathConstraints.size(); i++)
				{
					//We should filter here for the ea
					if (cur_ea == ponce_runtime_status.myPathConstraints[i].conditionAddr)
					{
						char name[256];
						//We put the index at the beginning so it is very easy to parse it with atoi(action_name)
						qsnprintf(name, 255, "%u_Ponce:solve_formula_sub", i);
						action_IDA_solve_formula_sub.name = name;
						char label[256];
						qsnprintf(label, 255, "%u. " MEM_FORMAT " -> " MEM_FORMAT, ponce_runtime_status.myPathConstraints[i].bound, ponce_runtime_status.myPathConstraints[i].conditionAddr, ponce_runtime_status.myPathConstraints[i].takenAddr);
						
						action_IDA_solve_formula_sub.label = label;
						bool success = register_action(action_IDA_solve_formula_sub);
						//If the submenu is already registered, we should unregister it and re-register it
						if (!success)
						{
							unregister_action(action_IDA_solve_formula_sub.name);
							success = register_action(action_IDA_solve_formula_sub);
						}
						success = attach_action_to_popup(form, popup_handle, action_IDA_solve_formula_sub.name, "SMT/Solve formula/", SETMENU_INS);
					}
				}
			}
			break;
		}
		case dbg_process_exit:
		{
			unhook_from_notification_point(HT_DBG, ui_callback, NULL);
			break;
		}
	}
	return 0;
}

/*We set the memory to the results we got and do the analysis from there*/
void set_SMT_results(Input *input_ptr){
	/*To set the memory types*/
	for (auto it = input_ptr->memOperand.begin(); it != input_ptr->memOperand.end(); it++)
	{
		auto concreteValue = api.getConcreteMemoryValue(*it, false);
		//auto concreteValue=it->getConcreteValue();
		put_bytes((ea_t)it->getAddress(), &concreteValue, it->getSize());

		api.setConcreteMemoryValue(*it, concreteValue);
		//We concretize the memory we set
		api.concretizeMemory(*it);
	}
		
	/*To set the register types*/
	for (auto it = input_ptr->regOperand.begin(); it != input_ptr->regOperand.end(); it++)
	{
		auto concreteRegValue = api.getConcreteRegisterValue(*it, false);
		set_reg_val(it->getName().c_str(), concreteRegValue.convert_to<uint64>());
		api.setConcreteRegisterValue(*it, concreteRegValue);
		//We concretize the register we set
		api.concretizeRegister(*it);
	}
		
	if (cmdOptions.showDebugInfo)
		msg("[+] Memory/Registers set with the SMT results\n");
}


#include "solver.hpp"
#include "globals.hpp"

#include <dbg.hpp>

/* This function return a vector of Inputs. A vector is necesary since switch conditions may have multiple branch constraints*/
std::vector<Input> solve_formula(ea_t pc, int bound)
{
    // Triton records every condition as a path constraint, we only care about the symbolized ones
    std::vector<const triton::engines::symbolic::PathConstraint*> symbolizedPathConstrains;
    for (auto &path_constrain : api.getPathConstraints()) {
        symbolizedPathConstrains.push_back(&path_constrain);
    }

    std::vector<Input> solutions;
    
    if (bound > symbolizedPathConstrains.size() - 1) {
        msg("Error. Requested bound %u is larger than PathConstraints vector size (%lu)\n", bound, symbolizedPathConstrains.size());
        return solutions;
    }

    // Double check that the condition at the bound is at the address the user selected
    assert(std::get<1>(symbolizedPathConstrains[bound]->getBranchConstraints()[0]) == pc);

    auto ast = api.getAstContext();
    // We are going to store here the constraints for the previous conditions
    // We can not initializate this to null, so we do it to a true condition (based on code_coverage_crackme_xor.py from the triton project)
    auto previousConstraints = ast->equal(ast->bvtrue(), ast->bvtrue());

    // Add user define constraints (borrar en reejecuccion, poner mensaje if not sat, 
    for (const auto& [id, user_constrain] : ponce_table_chooser->constrains) {
        previousConstraints = ast->land(previousConstraints, user_constrain);
    }

    // First we iterate through the previous path constrains to add the predicates of the taken path
    unsigned int j;
    for (j = 0; j < bound; j++)
    {
        if (cmdOptions.showExtraDebugInfo)
            msg("[+] Keeping condition %d\n", j);

        // We add to the previous constraints the predicate for the taken branch 
        auto predicate = symbolizedPathConstrains[j]->getTakenPredicate();
        previousConstraints = ast->land(previousConstraints, predicate);
    }

    // Then we use the predicate for the non taken path so we "solve" that condition.
    // We try to solve every non taken branch (more than one is possible under certain situations
    for (auto const& [taken, srcAddr, dstAddr, constraint] : symbolizedPathConstrains[bound]->getBranchConstraints()) {
        if (!taken) {
            // We concatenate the previous constraints for the taken path plus the non taken constrain of the user selected condition
            auto final_expr = ast->land(previousConstraints, constraint);
            if (cmdOptions.showExtraDebugInfo) {
                std::stringstream ss;
                /*Create the full formula*/
                ss << "(set-logic QF_AUFBV)\n";
                /* Then, delcare all symbolic variables */
                for (auto it : api.getSymbolicVariables()) {
                    ss << ast->declare(ast->variable(it.second));

                }
                /* And concat the user expression */
                ss << "\n\n";
                ss << final_expr;
                ss << "\n(check-sat)";
                ss << "\n(get-model)";
                msg("[+] Formula:\n%s\n\n", ss.str().c_str());
            }

            //Time to solve
            auto model = api.getModel(final_expr);

            if (model.size() > 0) {
                Input newinput;
                //Clone object 
                newinput.bound = bound;
                newinput.dstAddr = dstAddr;
                newinput.srcAddr = srcAddr;

                msg("[+] Solution found! Values:\n");
                for (const auto& [symId, model] : model) {
                    triton::engines::symbolic::SharedSymbolicVariable  symbVar = api.getSymbolicVariable(symId);
                    std::string  symbVarComment = symbVar->getComment();
                    triton::uint512 model_value = model.getValue();
                    if (symbVar->getType() == triton::engines::symbolic::variable_e::MEMORY_VARIABLE) {
                        auto mem = triton::arch::MemoryAccess(symbVar->getOrigin(), symbVar->getSize() / 8);
                        newinput.memOperand.push_back(mem);
                        api.setConcreteMemoryValue(mem, model_value); // Why
                    }
                    else if (symbVar->getType() == triton::engines::symbolic::variable_e::REGISTER_VARIABLE) {
                        auto reg = triton::arch::Register(*api.getCpuInstance(), (triton::arch::register_e)symbVar->getOrigin());
                        newinput.regOperand.push_back(reg);
                        api.setConcreteRegisterValue(reg, model_value); // Why?
                    }
                    //We represent the number different 
                    switch (symbVar->getSize())
                    {
                    case 8:
                        msg(" - %s (%s): %#02x (%c)\n", model.getVariable()->getName().c_str(), symbVarComment.c_str(), model_value.convert_to<uchar>(), model_value.convert_to<uchar>() == 0 ? ' ' : model_value.convert_to<uchar>());
                        break;
                    case 16:
                        msg(" - %s (%s): %#04x (%c%c)\n", model.getVariable()->getName().c_str(), symbVarComment.c_str(), model_value.convert_to<ushort>(), model_value.convert_to<uchar>() == 0 ? ' ' : model_value.convert_to<uchar>(), (unsigned char)(model_value.convert_to<ushort>() >> 8) == 0 ? ' ' : (unsigned char)(model_value.convert_to<ushort>() >> 8));
                        break;
                    case 32:
                        msg(" - %s (%s): %#08x\n", model.getVariable()->getName().c_str(), symbVarComment.c_str(), model_value.convert_to<uint32>());
                        break;
                    case 64:
                        msg(" - %s (%s): %#16llx\n", model.getVariable()->getName().c_str(), symbVarComment.c_str(), model_value.convert_to<uint64>());
                        break;
                    default:
                        msg("[!] Unsupported size for the symbolic variable: %s (%s)\n", model.getVariable()->getName().c_str(), symbVarComment.c_str()); // what about 128 - 512 registers? 
                    }
                }
                solutions.push_back(newinput);
            }
            else {
                msg("[!] No solution found :(\n");
            }
        }
    }
    return solutions;
}



/*This function identify the type of condition jmp and negate the flags to negate the jmp.
Probably it is possible to do this with the solver, adding more variable to the formula to
identify the flag of the conditions and get the values. But for now we are doing it in this way.*/
void negate_flag_condition(triton::arch::Instruction* triton_instruction)
{
    switch (triton_instruction->getType())
    {
    case triton::arch::x86::ID_INS_JA:
    {
        uint64 cf;
        get_reg_val("CF", &cf);
        uint64 zf;
        get_reg_val("ZF", &zf);
        if (cf == 0 && zf == 0)
        {
            cf = 1;
            zf = 1;
        }
        else
        {
            cf = 0;
            zf = 0;
        }
        set_reg_val("ZF", zf);
        set_reg_val("CF", cf);
        break;
    }
    case triton::arch::x86::ID_INS_JAE:
    {
        uint64 cf;
        get_reg_val("CF", &cf);
        uint64 zf;
        get_reg_val("ZF", &zf);
        if (cf == 0 || zf == 0)
        {
            cf = 1;
            zf = 1;
        }
        else
        {
            cf = 0;
            zf = 0;
        }
        set_reg_val("ZF", zf);
        set_reg_val("CF", cf);
        break;
    }
    case triton::arch::x86::ID_INS_JB:
    {
        uint64 cf;
        get_reg_val("CF", &cf);
        cf = !cf;
        set_reg_val("CF", cf);
        break;
    }
    case triton::arch::x86::ID_INS_JBE:
    {
        uint64 cf;
        get_reg_val("CF", &cf);
        uint64 zf;
        get_reg_val("ZF", &zf);
        if (cf == 1 || zf == 1)
        {
            cf = 0;
            zf = 0;
        }
        else
        {
            cf = 1;
            zf = 1;
        }
        set_reg_val("ZF", zf);
        set_reg_val("CF", cf);
        break;
    }
    /*	ToDo: Check this one
        case triton::arch::x86::ID_INS_JCXZ:
        {
        break;
        }*/
    case triton::arch::x86::ID_INS_JE:
    case triton::arch::x86::ID_INS_JNE:
    {
        uint64 zf;
        auto old_value = get_reg_val("ZF", &zf);
        zf = !zf;
        set_reg_val("ZF", zf);
        break;
    }
    //case triton::arch::x86::ID_INS_JRCXZ:
    //case triton::arch::x86::ID_INS_JECXZ:
    case triton::arch::x86::ID_INS_JG:
    {
        uint64 sf;
        get_reg_val("SF", &sf);
        uint64 of;
        get_reg_val("OF", &of);
        uint64 zf;
        get_reg_val("ZF", &zf);
        if (sf == of && zf == 0)
        {
            sf = !of;
            zf = 1;
        }
        else
        {
            sf = of;
            zf = 0;
        }
        set_reg_val("SF", sf);
        set_reg_val("OF", of);
        set_reg_val("ZF", zf);
        break;
    }
    case triton::arch::x86::ID_INS_JGE:
    {
        uint64 sf;
        get_reg_val("SF", &sf);
        uint64 of;
        get_reg_val("OF", &of);
        uint64 zf;
        get_reg_val("ZF", &zf);
        if (sf == of || zf == 1)
        {
            sf = !of;
            zf = 0;
        }
        else
        {
            sf = of;
            zf = 1;
        }
        set_reg_val("SF", sf);
        set_reg_val("OF", of);
        set_reg_val("ZF", zf);
        break;
    }
    case triton::arch::x86::ID_INS_JL:
    {
        uint64 sf;
        get_reg_val("SF", &sf);
        uint64 of;
        get_reg_val("OF", &of);
        if (sf == of)
        {
            sf = !of;
        }
        else
        {
            sf = of;
        }
        set_reg_val("SF", sf);
        set_reg_val("OF", of);
        break;
    }
    case triton::arch::x86::ID_INS_JLE:
    {
        uint64 sf;
        get_reg_val("SF", &sf);
        uint64 of;
        get_reg_val("OF", &of);
        uint64 zf;
        get_reg_val("ZF", &zf);
        if (sf != of || zf == 1)
        {
            sf = of;
            zf = 0;
        }
        else
        {
            sf = !of;
            zf = 1;
        }
        set_reg_val("SF", sf);
        set_reg_val("OF", of);
        set_reg_val("ZF", zf);
        break;
    }
    case triton::arch::x86::ID_INS_JNO:
    case triton::arch::x86::ID_INS_JO:
    {
        uint64 of;
        get_reg_val("OF", &of);
        of = !of;
        set_reg_val("OF", of);
        break;
    }
    case triton::arch::x86::ID_INS_JNP:
    case triton::arch::x86::ID_INS_JP:
    {
        uint64 pf;
        get_reg_val("PF", &pf);
        pf = !pf;
        set_reg_val("PF", pf);
        break;
    }
    case triton::arch::x86::ID_INS_JNS:
    case triton::arch::x86::ID_INS_JS:
    {
        uint64 sf;
        get_reg_val("SF", &sf);
        sf = !sf;
        set_reg_val("SF", sf);
        break;
    }
    default:
        msg("[!] We cannot negate %s instruction\n", triton_instruction->getDisassembly().c_str());
    }
}


/*We set the memory to the results we got and do the analysis from there*/
void set_SMT_solution(const Input& solution) {
    /*To set the memory types*/
    for (const auto& mem : solution.memOperand){
        auto concreteValue = api.getConcreteMemoryValue(mem, false);
        put_bytes((ea_t)mem.getAddress(), &concreteValue, mem.getSize());
        api.setConcreteMemoryValue(mem, concreteValue);

        if (cmdOptions.showExtraDebugInfo){
            char ascii_value[5] = { 0 };
            if(std::isprint(concreteValue.convert_to<unsigned char>()))
                qsnprintf(ascii_value, sizeof(ascii_value), "(%c)", concreteValue.convert_to<char>());
            std::stringstream stream;
            stream << std::hex << concreteValue;
            msg("[+] Memory " MEM_FORMAT " set with value 0x%s %s\n", 
                mem.getAddress(), 
                stream.str().c_str(), 
                std::isprint(concreteValue.convert_to<unsigned char>())? ascii_value :"");
        }
    }

    /*To set the register types*/
    for (const auto& reg : solution.regOperand) {
        auto concreteRegValue = api.getConcreteRegisterValue(reg, false);
        set_reg_val(reg.getName().c_str(), concreteRegValue.convert_to<uint64>());
        api.setConcreteRegisterValue(reg, concreteRegValue);

        if (cmdOptions.showExtraDebugInfo) {
            char ascii_value[5] = { 0 };
            if (std::isprint(concreteRegValue.convert_to<unsigned char>()))
                qsnprintf(ascii_value, sizeof(ascii_value), "(%c)", concreteRegValue.convert_to<char>());
            std::stringstream stream;
            stream << std::hex << concreteRegValue;

            msg("[+] Registers %s set with value 0x%s %s\n", 
                reg.getName().c_str(), 
                stream.str().c_str(),
                std::isprint(concreteRegValue.convert_to<unsigned char>()) ? ascii_value : "");
        }
    }

    if (cmdOptions.showDebugInfo)
        msg("[+] Memory/Registers set with the SMT results\n");
}
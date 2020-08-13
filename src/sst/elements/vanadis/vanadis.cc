

#include <sst_config.h>
#include <sst/core/output.h>

#include "vanadis.h"

#include "velf/velfinfo.h"
#include "decoder/vmipsdecoder.h"
#include "inst/vinstall.h"

using namespace SST::Vanadis;

VanadisComponent::VanadisComponent(SST::ComponentId_t id, SST::Params& params) :
	Component(id),
	current_cycle(0) {
	

	instPrintBuffer = new char[1024];

	max_cycle = params.find<uint64_t>("max_cycle",
		std::numeric_limits<uint64_t>::max() );

	const int32_t verbosity = params.find<int32_t>("verbose", 0);
	core_id   = params.find<uint32_t>("core_id", 0);

	char* outputPrefix = (char*) malloc( sizeof(char) * 256 );
	sprintf(outputPrefix, "[Core: %6" PRIu32 "]: ", core_id);

	output = new SST::Output(outputPrefix, verbosity, 0, Output::STDOUT);
	free(outputPrefix);

	const char* check_bin = "./tests/hello-mips";
	VanadisELFInfo* bin_info = readBinaryELFInfo( output, check_bin );
	bin_info->print( output );

	std::string clock_rate = params.find<std::string>("clock", "1GHz");
	output->verbose(CALL_INFO, 2, 0, "Registering clock at %s.\n", clock_rate.c_str());
	cpuClockTC = registerClock( clock_rate, new Clock::Handler<VanadisComponent>(this, &VanadisComponent::tick) );

	const uint32_t rob_count = params.find<uint32_t>("reorder_slots", 64);
	dCacheLineWidth = params.find<uint64_t>("dcache_line_width", 64);
        iCacheLineWidth = params.find<uint64_t>("icache_line_width", 64);

	output->verbose(CALL_INFO, 2, 0, "Core L1 Cache Configurations:\n");
	output->verbose(CALL_INFO, 2, 0, "-> D-Cache Line Width:       %" PRIu64 " bytes\n", dCacheLineWidth);
	output->verbose(CALL_INFO, 2, 0, "-> I-Cache Line Width:       %" PRIu64 " bytes\n", iCacheLineWidth);

	hw_threads = params.find<uint32_t>("hardware_threads", 1);
	output->verbose(CALL_INFO, 2, 0, "Creating %" PRIu32 " SMT threads.\n", hw_threads);

        print_int_reg = params.find<bool>("print_int_reg", verbosity > 16 ? 1 : 0);
        print_fp_reg  = params.find<bool>("print_fp_reg",  verbosity > 16 ? 1 : 0);

	const uint16_t int_reg_count = params.find<uint16_t>("physical_integer_registers", 128);
	const uint16_t fp_reg_count  = params.find<uint16_t>("physical_fp_registers", 128);

	output->verbose(CALL_INFO, 2, 0, "Creating physical register files (quantities are per hardware thread)...\n");
	output->verbose(CALL_INFO, 2, 0, "Physical Integer Registers (GPRs): %5" PRIu16 "\n", int_reg_count);
	output->verbose(CALL_INFO, 2, 0, "Physical Floating-Point Registers: %5" PRIu16 "\n", fp_reg_count);

	const uint16_t issue_queue_len = params.find<uint16_t>("issue_queue_length", 4);

	halted_masks = new bool[hw_threads];

	const uint32_t branch_entries = params.find<uint32_t>("branch_predict_entries", 32);
	output->verbose(CALL_INFO, 2, 0, "Branch prediction entries:         %10" PRIu32 "\n", branch_entries);

	//////////////////////////////////////////////////////////////////////////////////////

	char* decoder_name = new char[64];

	for( uint32_t i = 0; i < hw_threads; ++i ) {

		sprintf(decoder_name, "decoder%" PRIu32 "", i);
		VanadisDecoder* thr_decoder = loadUserSubComponent<SST::Vanadis::VanadisDecoder>(decoder_name);
		output->verbose(CALL_INFO, 8, 0, "Loading decoder%" PRIu32 ": %s.\n", i,
			(nullptr == thr_decoder) ? "failed" : "successful");

		if( nullptr == thr_decoder ) {
			output->fatal(CALL_INFO, -1, "Error: was unable to load %s on thread %" PRIu32 "\n",
				decoder_name, i);
		} else {
			output->verbose(CALL_INFO, 8, 0, "-> Decoder configured for %s\n",
				thr_decoder->getISAName());
		}

		thread_decoders.push_back( thr_decoder );

		if( 0 == thread_decoders[i]->getInsCacheLineWidth() ) {
			output->verbose(CALL_INFO, 2, 0, "Auto-setting icache line width in decoder to %" PRIu64 "\n", iCacheLineWidth);
			thread_decoders[i]->setInsCacheLineWidth( iCacheLineWidth );
		} else {
			if( iCacheLineWidth < thread_decoders[i]->getInsCacheLineWidth() ) {
				output->fatal(CALL_INFO, -1, "Decoder for thr %" PRIu32 " has an override icache-line-width of %" PRIu64 ", this exceeds the core icache-line-with of %" PRIu64 " and is likely to result in cache load failures. Set this to less than equal to %" PRIu64 "\n",
					i, thread_decoders[i]->getInsCacheLineWidth(), iCacheLineWidth, iCacheLineWidth);
			} else {
				output->verbose(CALL_INFO, 2, 0, "Decoder for thr %" PRIu32 " is already set to %" PRIu64 ", will not auto-set. The core icache-line-width is currently: %" PRIu64 "\n",
					(uint32_t) i, thread_decoders[i]->getInsCacheLineWidth(),
					iCacheLineWidth);
			}
		}

		isa_options.push_back( thread_decoders[i]->getDecoderOptions() );

		output->verbose(CALL_INFO, 8, 0, "Thread: %6" PRIu32 " ISA set to: %s [Int-Reg: %" PRIu16 "/FP-Reg: %" PRIu16 "]\n", 
			i,
			thread_decoders[i]->getISAName(),
			thread_decoders[i]->countISAIntReg(),
			thread_decoders[i]->countISAFPReg());

		register_files.push_back( new VanadisRegisterFile( i, thread_decoders[i]->getDecoderOptions(),
			int_reg_count, fp_reg_count ) );
		int_register_stacks.push_back( new VanadisRegisterStack( int_reg_count ) );
		fp_register_stacks.push_back( new VanadisRegisterStack( fp_reg_count ));

		output->verbose(CALL_INFO, 8, 0, "Reorder buffer set to %" PRIu32 " entries, these are shared by all threads.\n",
			rob_count);
		rob.push_back( new VanadisCircularQueue<VanadisInstruction*>( rob_count ) );
		// WE NEED ISA INTEGER AND FP COUNTS HERE NOT ZEROS
		issue_isa_tables.push_back( new VanadisISATable( thread_decoders[i]->getDecoderOptions(),
			thread_decoders[i]->countISAIntReg(), thread_decoders[i]->countISAFPReg() ) );

		branch_units.push_back( new VanadisBranchUnit( branch_entries ) );
		thread_decoders[i]->setBranchUnit( branch_units[i] );

		for( uint16_t j = 0; j < thread_decoders[i]->countISAIntReg(); ++j ) {
			issue_isa_tables[i]->setIntPhysReg( j, int_register_stacks[i]->pop() );
		}

		for( uint16_t j = 0; j < thread_decoders[i]->countISAFPReg(); ++j ) {
			issue_isa_tables[i]->setFPPhysReg( j, fp_register_stacks[i]->pop() );
		}

		retire_isa_tables.push_back( new VanadisISATable( thread_decoders[i]->getDecoderOptions(),
			thread_decoders[i]->countISAIntReg(), thread_decoders[i]->countISAFPReg() ) );
		retire_isa_tables[i]->reset(issue_isa_tables[i]);

		halted_masks[i] = true;
	}

	delete[] decoder_name;

	if( 0 == core_id ) {
		halted_masks[0] = false;

		if( 0 == thread_decoders[0]->getInstructionPointer() ) {
			// This wasn't provided, or its explicitly set to zero which means
			// we should auto-calculate it
			const uint64_t c0_entry = bin_info->getEntryPoint();
			output->verbose(CALL_INFO, 8, 0, "Configuring core-0, thread-0 entry point = %p\n", 
				(void*) c0_entry);
			thread_decoders[0]->setInstructionPointer( c0_entry );
		} else {
			output->verbose(CALL_INFO, 8, 0, "Entry point for core-0, thread-0 is set by configuration or decoder to: %p\n",
				(void*) thread_decoders[0]->getInstructionPointer());
		}
	}

	VanadisInstruction* test_ins = new VanadisAddInstruction(nextInsID++, 0, 0, isa_options[0], 3, 4, 5);
	thread_decoders[0]->getDecodedQueue()->push( test_ins );
	rob[0]->push(test_ins);

	test_ins = new VanadisAddImmInstruction(nextInsID++, 1, 0, isa_options[0], 1, 3, 128);
	thread_decoders[0]->getDecodedQueue()->push( test_ins );
	rob[0]->push(test_ins);

//	test_ins = new VanadisAddImmInstruction(nextInsID++, 2, 0, isa_options[0], 3, 10101010, 4);
//	thread_decoders[0]->getDecodedQueue()->push( test_ins );
//	rob[0]->push(test_ins);

//	test_ins = new VanadisAddInstruction(nextInsID++, 4, 0, isa_options[0], 9, 4, 3);
//	thread_decoders[0]->getDecodedQueue()->push( test_ins );
//	rob[0]->push(test_ins);
	
	test_ins = new VanadisSubInstruction(nextInsID++, 3, 0, isa_options[0], 4, 1, 1);
	thread_decoders[0]->getDecodedQueue()->push( test_ins );
	rob[0]->push(test_ins);

	test_ins = new VanadisSubInstruction(nextInsID++, 3, 0, isa_options[0], 5, 6, 1);
	thread_decoders[0]->getDecodedQueue()->push( test_ins );
	rob[0]->push(test_ins);

	test_ins = new VanadisAddImmInstruction(nextInsID++, 3, 0, isa_options[0], 10, 0, 256 );
	thread_decoders[0]->getDecodedQueue()->push( test_ins );
        rob[0]->push(test_ins);

	test_ins = new VanadisStoreInstruction( nextInsID++, 3, 0, isa_options[0], 10, 512, 5, 8 );
	thread_decoders[0]->getDecodedQueue()->push( test_ins );
        rob[0]->push(test_ins);

	test_ins = new VanadisLoadInstruction( nextInsID++, 4, 0, isa_options[0], 0, 768, 12, 8 );
	thread_decoders[0]->getDecodedQueue()->push( test_ins );
        rob[0]->push(test_ins);

	//////////////////////////////////////////////////////////////////////////////////////

	uint16_t fu_id = 0;

	const uint16_t int_arith_units = params.find<uint16_t>("integer_arith_units", 2);
	const uint16_t int_arith_cycles = params.find<uint16_t>("integer_arith_cycles", 2);

	output->verbose(CALL_INFO, 2, 0, "Creating %" PRIu16 " integer arithmetic units, latency = %" PRIu16 "...\n",
		int_arith_units, int_arith_cycles);

	for( uint16_t i = 0; i < int_arith_units; ++i ) {
		fu_int_arith.push_back( new VanadisFunctionalUnit(fu_id++, INST_INT_ARITH, int_arith_cycles) );
	}

	const uint16_t int_div_units = params.find<uint16_t>("integer_div_units", 1);
	const uint16_t int_div_cycles = params.find<uint16_t>("integer_div_cycles", 4);

	output->verbose(CALL_INFO, 2, 0, "Creating %" PRIu16 " integer division units, latency = %" PRIu16 "...\n",
		int_div_units, int_div_cycles);

	for( uint16_t i = 0; i < int_div_units; ++i ) {
		fu_int_div.push_back( new VanadisFunctionalUnit(fu_id++, INST_INT_DIV, int_div_cycles) );
	}

	//////////////////////////////////////////////////////////////////////////////////////

	const uint16_t fp_arith_units = params.find<uint16_t>("fp_arith_units", 2);
	const uint16_t fp_arith_cycles = params.find<uint16_t>("fp_arith_cycles", 8);

	output->verbose(CALL_INFO, 2, 0, "Creating %" PRIu16 " floating point arithmetic units, latency = %" PRIu16 "...\n",
		fp_arith_units, fp_arith_cycles);

	for( uint16_t i = 0; i < fp_arith_units; ++i ) {
		fu_fp_arith.push_back( new VanadisFunctionalUnit(fu_id++, INST_FP_ARITH, fp_arith_cycles) );
	}

	const uint16_t fp_div_units = params.find<uint16_t>("fp_div_units", 1);
	const uint16_t fp_div_cycles = params.find<uint16_t>("fp_div_cycles", 80);

	output->verbose(CALL_INFO, 2, 0, "Creating %" PRIu16 " floating point division units, latency = %" PRIu16 "...\n",
		fp_div_units, fp_div_cycles);

	for( uint16_t i = 0; i < fp_div_units; ++i ) {
		fu_fp_div.push_back( new VanadisFunctionalUnit(fu_id++, INST_FP_DIV, fp_div_cycles) );
	}

	//////////////////////////////////////////////////////////////////////////////////////

	memDataInterface = loadUserSubComponent<Interfaces::SimpleMem>("mem_interface_data", ComponentInfo::SHARE_NONE, cpuClockTC, new SimpleMem::Handler<SST::Vanadis::VanadisComponent>(this, &VanadisComponent::handleIncomingDataCacheEvent ));
	memInstInterface = loadUserSubComponent<Interfaces::SimpleMem>("mem_interface_inst", ComponentInfo::SHARE_NONE, cpuClockTC, new SimpleMem::Handler<SST::Vanadis::VanadisComponent>(this, &VanadisComponent::handleIncomingInstCacheEvent));

    	// Load anonymously if not found in config
/*
    	if (!memInterface) {
        	std::string memIFace = params.find<std::string>("meminterface", "memHierarchy.memInterface");
        	output.verbose(CALL_INFO, 1, 0, "Loading memory interface: %s ...\n", memIFace.c_str());
        	Params interfaceParams = params.find_prefix_params("meminterface.");
        	interfaceParams.insert("port", "dcache_link");

        	memInterface = loadAnonymousSubComponent<Interfaces::SimpleMem>(memIFace, "memory", 0, ComponentInfo::SHARE_PORTS | ComponentInfo::INSERT_STATS,
		interfaceParams, cpuClockTC, new SimpleMem::Handler<JunoCPU>(this, &JunoCPU::handleEvent));

        	if( NULL == mem )
            		output.fatal(CALL_INFO, -1, "Error: unable to load %s memory interface.\n", memIFace.c_str());
    	}
*/

	if( nullptr == memDataInterface ) {
		output->fatal(CALL_INFO, -1, "Error: unable to load memory interface subcomponent for data cache.\n");
	}

	if( nullptr == memInstInterface ) {
		output->fatal(CALL_INFO, -1, "Error: unable ot load memory interface subcomponent for instruction cache.\n");
	}

    	output->verbose(CALL_INFO, 1, 0, "Successfully loaded memory interface.\n");

	size_t lsq_store_size    = params.find<size_t>("lsq_store_entries", 8);
	size_t lsq_store_pending = params.find<size_t>("lsq_issued_stores_inflight", 8);
	size_t lsq_load_size     = params.find<size_t>("lsq_load_entries", 8);
	size_t lsq_load_pending  = params.find<size_t>("lsq_issused_loads_inflight", 8);
	size_t lsq_max_loads_per_cycle = params.find<size_t>("max_loads_per_cycle", 2);
	size_t lsq_max_stores_per_cycle = params.find<size_t>("max_stores_per_cycle", 2);

	lsq = new VanadisLoadStoreQueue( memDataInterface, lsq_store_size, lsq_store_pending,
		lsq_load_size, lsq_load_pending, lsq_max_loads_per_cycle, 
		lsq_max_stores_per_cycle, &register_files);

	registerAsPrimaryComponent();
    	primaryComponentDoNotEndSim();
}

VanadisComponent::~VanadisComponent() {
	delete[] instPrintBuffer;
	delete lsq;
}

bool VanadisComponent::tick(SST::Cycle_t cycle) {
	if( current_cycle >= max_cycle ) {
		output->verbose(CALL_INFO, 1, 0, "Reached maximum cycle %" PRIu64 ". Core stops processing.\n", current_cycle );
		primaryComponentOKToEndSim();
		return true;
	}

	bool should_process = false;
	for( uint32_t i = 0; i < hw_threads; ++i ) {
		should_process = should_process | halted_masks[i];
	}

	output->verbose(CALL_INFO, 2, 0, "============================ Cycle %12" PRIu64 " ============================\n", current_cycle );

	output->verbose(CALL_INFO, 8, 0, "-- Core Status:\n");

	for( uint32_t i = 0; i < hw_threads; ++i) {
		output->verbose(CALL_INFO, 8, 0, "---> Thr: %5" PRIu32 " (%s) / ROB-Pend: %" PRIu16 " / IntReg-Free: %" PRIu16 " / FPReg-Free: %" PRIu16 "\n", 
			i, halted_masks[i] ? "halted" : "unhalted", 
			(uint16_t) rob[i]->size(),
			(uint16_t) int_register_stacks[i]->unused(), (uint16_t) fp_register_stacks[i]->unused() );
	}

	// Fetch
	output->verbose(CALL_INFO, 8, 0, "-- Fetch Stage --------------------------------------------------------------\n");
	for( uint32_t i = 0; i < hw_threads; ++i ) {
		if( ! halted_masks[i] ) {
			if( thread_decoders[i]->requestingDelegatedRead() ) {
				const uint64_t del_addr  = thread_decoders[i]->getDelegatedLoadAddr();
				const uint16_t del_width = thread_decoders[i]->getDelegatedLoadWidth();

				output->verbose(CALL_INFO, 16, 0, "-> Thr: %" PRIu32 " is requesting a delegated i-cache read, addr=%p, width=%" PRIu16"\n",
					i, (void*) del_addr, del_width);

				// Clear out the load, we have to wait for the cache to process it now
				thread_decoders[i]->clearDelegatedLoadRequest();
			}
		}
	}

	// Decode
	output->verbose(CALL_INFO, 8, 0, "-- Decode Stage -------------------------------------------------------------\n");
	for( uint32_t i = 0 ; i < hw_threads; ++i ) {
		// If thread is not masked then decode from it
		if( ! halted_masks[i] ) {
			thread_decoders[i]->tick(output, (uint64_t) cycle);
		}
	}

	if( thread_decoders[0]->getDecodedQueue()->size() < 6 ) {
		VanadisInstruction* test_ins = new VanadisAddInstruction(nextInsID++, 0, 0, isa_options[0], 0, 0, 0);
	        thread_decoders[0]->getDecodedQueue()->push( test_ins );
        	rob[0]->push(test_ins);
	}

	// Issue
	output->verbose(CALL_INFO, 8, 0, "-- Issue Stage --------------------------------------------------------------\n");
	for( uint32_t i = 0 ; i < hw_threads; ++i ) {
		tmp_raw_int.clear();
		tmp_raw_fp.clear();

		// If thread is not masked then pull a pending instruction and issue
		if( ! halted_masks[i] ) {
			output->verbose(CALL_INFO, 8, 0, "--> Performing issue for thread %" PRIu32 " (decoded pending queue depth: %" PRIu32 ")...\n",
				i, (uint32_t) thread_decoders[i]->getDecodedQueue()->size());

			if( ! thread_decoders[i]->getDecodedQueue()->empty() ) {
				VanadisInstruction* ins = thread_decoders[i]->getDecodedQueue()->peek();
				ins->printToBuffer(instPrintBuffer, 1024);
				
				output->verbose(CALL_INFO, 8, 0, "--> Attempting issue for: %s / %p\n", instPrintBuffer,
						(void*) ins->getInstructionAddress());


				const int resource_check = checkInstructionResources( ins,
					int_register_stacks[i], fp_register_stacks[i],
					issue_isa_tables[i], tmp_raw_int, tmp_raw_fp);

				output->verbose(CALL_INFO, 8, 0, "Instruction resource can be issuable: %s (issue-query result: %d)\n",
					(resource_check == 0) ? "yes" : "no", resource_check);

				bool can_be_issued = (resource_check == 0);
				bool allocated_fu = false;

				// Register dependencies are met and ROB has an entry
				if( can_be_issued ) {

					const VanadisFunctionalUnitType ins_type = ins->getInstFuncType();

					switch( ins_type ) {
					case INST_INT_ARITH:
						for( VanadisFunctionalUnit* next_fu : fu_int_arith ) {
							if(next_fu->isInstructionSlotFree()) {
								next_fu->setSlotInstruction( ins );
								allocated_fu = true;
								break;				
							}
						}

						break;
					case INST_FP_ARITH:
						for( VanadisFunctionalUnit* next_fu : fu_fp_div ) {
							if(next_fu->isInstructionSlotFree()) {
								next_fu->setSlotInstruction( ins );
								allocated_fu = true;
								break;				
							}
						}

						break;
					case INST_LOAD:
						if( ! lsq->loadFull() ) {
							lsq->push( (VanadisLoadInstruction*) ins );
							allocated_fu = true;
						}
						break;
					case INST_STORE:
						if( ! lsq->storeFull() ) {
							lsq->push( (VanadisStoreInstruction*) ins );
							allocated_fu = true;
						}
						break;
					case INST_INT_DIV:
						for( VanadisFunctionalUnit* next_fu : fu_int_div ) {
							if(next_fu->isInstructionSlotFree()) {
								next_fu->setSlotInstruction( ins );
								allocated_fu = true;
								break;				
							}
						}

						break;
					case INST_FP_DIV:
						for( VanadisFunctionalUnit* next_fu : fu_fp_div ) {
							if(next_fu->isInstructionSlotFree()) {
								next_fu->setSlotInstruction( ins );
								allocated_fu = true;
								break;				
							}
						}

						break;
					default:
						// ERROR UNALLOCATED
						output->fatal(CALL_INFO, -1, "Error - no processing for instruction class.\n");
						break;
					}

					if( allocated_fu ) {
						const int status = assignRegistersToInstruction(ins,
							int_register_stacks[i],
							fp_register_stacks[i],
							issue_isa_tables[i]);

						thread_decoders[i]->getDecodedQueue()->pop();
						ins->markIssued();
						output->verbose(CALL_INFO, 8, 0, "Issued to functional unit, status=%d\n", status);
					}
				}
			}
		}

		issue_isa_tables[i]->print(output, register_files[i], print_int_reg, print_fp_reg);
	}

	// Functional Units / Execute
	output->verbose(CALL_INFO, 8, 0, "-- Execute Stage ------------------------------------------------------------\n");

	for( VanadisFunctionalUnit* next_fu : fu_int_arith ) {
		next_fu->tick(cycle, output, register_files);
	}

	for( VanadisFunctionalUnit* next_fu : fu_int_div ) {
		next_fu->tick(cycle, output, register_files);
	}

	for( VanadisFunctionalUnit* next_fu : fu_fp_arith ) {
		next_fu->tick(cycle, output, register_files);
	}

	for( VanadisFunctionalUnit* next_fu : fu_fp_div ) {
		next_fu->tick(cycle, output, register_files);
	}

	// LSQ Processing
	lsq->tick( (uint64_t) cycle, output );

	// Retirement
	output->verbose(CALL_INFO, 8, 0, "-- Retire Stage -------------------------------------------------------------\n");

	for( uint32_t i = 0; i < hw_threads; ++i ) {
		output->verbose(CALL_INFO, 8, 0, "Executing retire for thread %" PRIu32 "...\n", i);

		if( ! rob[i]->empty() ) {
			VanadisInstruction* rob_front = rob[i]->peek();

			// Instruction is flagging error, print out and halt
			if( rob_front->trapsError() ) {
				output->fatal( CALL_INFO, -1, "Instruction %" PRIu64 " at %" PRIu64 " flags an error (instruction-type=%s)\n",
					rob_front->getID(), rob_front->getInstructionAddress(),
					rob_front->getInstCode() );
			}

			if( rob_front->isSpeculated() && rob_front->completedExecution() ) {
				// Check we predicted in the right direction.
				output->verbose(CALL_INFO, 8, 0, "ROB -> front on thread %" PRIu32 " is a speculated instruction.\n", i);

				VanadisSpeculatedInstruction* spec_ins = dynamic_cast<VanadisSpeculatedInstruction*>( rob_front );

				output->verbose(CALL_INFO, 8, 0, "ROB -> check prediction: speculated: %s / result: %s\n",
					directionToChar( spec_ins->getSpeculatedDirection() ),
					directionToChar( spec_ins->getResultDirection( register_files[i] ) ) );

				if( spec_ins->getSpeculatedDirection() != spec_ins->getResultDirection( register_files[i] ) ) {
					// We have a mis-speculated instruction, uh-oh.
					output->verbose(CALL_INFO, 8, 0, "ROB -> mis-speculated execution, begin pipeline reset.\n");
				} else {
					output->verbose(CALL_INFO, 8, 0, "ROB -> speculation correct.\n");
				}
			} else if( rob_front->completedExecution() ) {
				output->verbose(CALL_INFO, 8, 0, "ROB for Thread %5" PRIu32 " contains entries and those have finished executing, in retire status...\n", i);

				// Actually pop the instruction now we know its safe to do so.
				rob[i]->pop();

				recoverRetiredRegisters( rob_front,
					int_register_stacks[rob_front->getHWThread()],
					fp_register_stacks[rob_front->getHWThread()],
					issue_isa_tables[i], retire_isa_tables[i] );

				retire_isa_tables[i]->print(output, print_int_reg, print_fp_reg);

				delete rob_front;
			} else {
				// make sure instruction is marked at front of ROB since this can
				// enable instructions which need to be retire-ready to process
				rob_front->markFrontOfROB();
			}
		}
	}

	output->verbose(CALL_INFO, 2, 0, "================================ End of Cycle ==============================\n" );

	current_cycle++;
	return false;
}

int VanadisComponent::checkInstructionResources(
	VanadisInstruction* ins,
    VanadisRegisterStack* int_regs,
    VanadisRegisterStack* fp_regs,
    VanadisISATable* isa_table,
	std::set<uint16_t>& isa_int_regs_written_ahead,
	std::set<uint16_t>& isa_fp_regs_written_ahead) {

	bool resources_good = true;

	// We need places to store our output registers
	resources_good &= (int_regs->unused() >= ins->countISAIntRegOut());
	resources_good &= (fp_regs->unused() >= ins->countISAFPRegOut());

	// If there are any pending writes against our reads, we can't issue until
	// they are done
	for( uint16_t i = 0; i < ins->countISAIntRegIn(); ++i ) {
		const uint16_t ins_isa_reg = ins->getISAIntRegIn(i);
		resources_good &= (!isa_table->pendingIntWrites(ins_isa_reg));

		// Check there are no RAW in the pending instruction queue
		resources_good &= (isa_int_regs_written_ahead.find(ins_isa_reg) == isa_int_regs_written_ahead.end());
	}
	
	output->verbose(CALL_INFO, 16, 0, "--> Check input integer registers, issue-status: %s\n",
		(resources_good ? "yes" : "no") );

	if( resources_good ) {
		for( uint16_t i = 0; i < ins->countISAFPRegIn(); ++i ) {
			const uint16_t ins_isa_reg = ins->getISAFPRegIn(i);
			resources_good &= (!isa_table->pendingFPWrites(ins_isa_reg));

			// Check there are no RAW in the pending instruction queue
			resources_good &= (isa_fp_regs_written_ahead.find(ins_isa_reg) == isa_fp_regs_written_ahead.end());
		}
	
		output->verbose(CALL_INFO, 16, 0, "--> Check input floating-point registers, issue-status: %s\n",
			(resources_good ? "yes" : "no") );
	}

	// Update RAW table
	for( uint16_t i = 0; i < ins->countISAIntRegOut(); ++i ) {
		const uint16_t ins_isa_reg = ins->getISAIntRegOut(i);
		isa_int_regs_written_ahead.insert(ins_isa_reg);
	}

	for( uint16_t i = 0; i < ins->countISAFPRegOut(); ++i ) {
		const uint16_t ins_isa_reg = ins->getISAIntRegOut(i);
		isa_fp_regs_written_ahead.insert(ins_isa_reg);
	}

	return (resources_good) ? 0 : 1;
}

int VanadisComponent::assignRegistersToInstruction(
        VanadisInstruction* ins,
        VanadisRegisterStack* int_regs,
        VanadisRegisterStack* fp_regs,
        VanadisISATable* isa_table) {

	// Set the current ISA registers required for input
	for( uint16_t i = 0; i < ins->countISAIntRegIn(); ++i ) {
		ins->setPhysIntRegIn(i, isa_table->getIntPhysReg( ins->getISAIntRegIn(i) ));
		isa_table->incIntRead( ins->getISAIntRegIn(i) );
	}

	for( uint16_t i = 0; i < ins->countISAFPRegIn(); ++i ) {
		ins->setPhysFPRegIn(i, isa_table->getFPPhysReg( ins->getISAFPRegIn(i) ));
		isa_table->incFPRead( ins->getISAFPRegIn(i) );
	}

	// Set current ISA registers required for output
	for( uint16_t i = 0; i < ins->countISAIntRegOut(); ++i ) {
		const uint16_t ins_isa_reg = ins->getISAIntRegOut(i);
		const uint16_t free_reg = int_regs->pop();

		ins->setPhysIntRegOut(i, free_reg);
		isa_table->setIntPhysReg( ins_isa_reg, free_reg );
		isa_table->incIntWrite( ins_isa_reg );
	}

	// Set current ISA registers required for output
	for( uint16_t i = 0; i < ins->countISAFPRegOut(); ++i ) {
		const uint16_t ins_isa_reg = ins->getISAFPRegOut(i);
		const uint16_t free_reg = fp_regs->pop();

		ins->setPhysFPRegOut(i, free_reg);
		isa_table->setFPPhysReg( ins_isa_reg, free_reg );
		isa_table->incFPWrite( ins_isa_reg );
	}

	return 0;
}

int VanadisComponent::recoverRetiredRegisters(
        VanadisInstruction* ins,
        VanadisRegisterStack* int_regs,
        VanadisRegisterStack* fp_regs,
	VanadisISATable* issue_isa_table,
        VanadisISATable* retire_isa_table) {

	std::vector<uint16_t> recovered_phys_reg_int;
	std::vector<uint16_t> recovered_phys_reg_fp;

	for( uint16_t i = 0; i < ins->countISAIntRegIn(); ++i ) {
		const uint16_t isa_reg = ins->getISAIntRegIn(i);
		issue_isa_table->decIntRead( isa_reg );
	}

	for( uint16_t i = 0; i < ins->countISAFPRegIn(); ++i ) {
		const uint16_t isa_reg = ins->getISAFPRegIn(i);
		issue_isa_table->decFPRead( isa_reg );
	}

	for( uint16_t i = 0; i < ins->countISAIntRegOut(); ++i ) {
		const uint16_t isa_reg = ins->getISAIntRegOut(i);
   		const uint16_t cur_phys_reg = retire_isa_table->getIntPhysReg(isa_reg);
		
		recovered_phys_reg_int.push_back( cur_phys_reg );

		issue_isa_table->decIntWrite( isa_reg );

		// Set the ISA register in the retirement table to point
		// to the physical register used by this instruction
		retire_isa_table->setIntPhysReg( isa_reg, ins->getPhysIntRegOut(i) );
	}

	for( uint16_t i = 0; i < ins->countISAFPRegOut(); ++i ) {
		const uint16_t isa_reg = ins->getISAFPRegOut(i);
		const uint16_t cur_phys_reg = retire_isa_table->getFPPhysReg(isa_reg);

		recovered_phys_reg_fp.push_back( cur_phys_reg );

		issue_isa_table->decFPWrite( isa_reg );

		// Set the ISA register in the retirement table to point
		// to the physical register used by this instruction
		retire_isa_table->setFPPhysReg( isa_reg, ins->getPhysFPRegOut(i) );
	}

	output->verbose(CALL_INFO, 16, 0, "Recovering: %d int-reg and %d fp-reg\n",
		(int) recovered_phys_reg_int.size(), (int) recovered_phys_reg_fp.size());

	for( uint16_t next_reg : recovered_phys_reg_int ) {
		int_regs->push(next_reg);	
	}

	for( uint16_t next_reg : recovered_phys_reg_fp ) {
		fp_regs->push(next_reg);
	}

	return 0;
}

void VanadisComponent::setup() {

}

void VanadisComponent::finish() {

}

void VanadisComponent::printStatus() {

}

void VanadisComponent::init(unsigned int phase) {

}

void VanadisComponent::handleIncomingDataCacheEvent( SimpleMem::Request* ev ) {
	output->verbose(CALL_INFO, 16, 0, "-> D-Cache Incoming Event\n");
	lsq->processIncomingDataCacheEvent( output, ev );
}

void VanadisComponent::handleIncomingInstCacheEvent( SimpleMem::Request* ev ) {
	output->verbose(CALL_INFO, 16, 0, "-> I-Cache Incoming Event\n");
	// Needs to get attached to the decoder
}

void VanadisComponent::handleMisspeculate( const uint32_t hw_thr ) {
	output->verbose(CALL_INFO, 16, 0, "-> Handle mis-speculation on %" PRIu32 "...\n", hw_thr);

	clearFuncUnit( hw_thr, fu_int_arith );
	clearFuncUnit( hw_thr, fu_int_div );
	clearFuncUnit( hw_thr, fu_fp_arith );
	clearFuncUnit( hw_thr, fu_fp_div );

	lsq->clearLSQByThreadID( output, hw_thr );
	resetRegisterStacks( hw_thr );
	clearROBMisspeculate(hw_thr);

	// Reset the ISA table to get correct ISA to physical mappings
	issue_isa_tables[hw_thr]->reset( retire_isa_tables[hw_thr] );

	output->verbose(CALL_INFO, 16, 0, "-> Mis-speculate repair finished.\n");
}

void VanadisComponent::clearFuncUnit( const uint32_t hw_thr, std::vector<VanadisFunctionalUnit*>& unit ) {
	for( VanadisFunctionalUnit* next_fu : unit ) {
		next_fu->clearByHWThreadID( output, hw_thr );
	}
}

void VanadisComponent::resetRegisterStacks( const uint32_t hw_thr ) {
	output->verbose(CALL_INFO, 16, 0, "-> Resetting register stacks on thread %" PRIu32 "...\n", hw_thr);

	output->verbose(CALL_INFO, 16, 0, "---> Reclaiming integer registers...\n");

	const uint16_t int_reg_count = int_register_stacks[hw_thr]->capacity();
	VanadisRegisterStack* new_int_stack = new VanadisRegisterStack( int_reg_count );

	for( uint16_t i = 0; i < int_reg_count; ++i ) {
		if( ! retire_isa_tables[ hw_thr ]->physIntRegInUse( i ) ) {
			new_int_stack->push(i);
		}
	}

	delete int_register_stacks[hw_thr];
	int_register_stacks[hw_thr] = new_int_stack;

	output->verbose(CALL_INFO, 16, 0, "---> Integer register stack contains %" PRIu32 " registers.\n",
		(uint32_t) new_int_stack->size());
	output->verbose(CALL_INFO, 16, 0, "---> Reclaiming floating point registers...\n");

	const uint16_t fp_reg_count = fp_register_stacks[hw_thr]->capacity();
	VanadisRegisterStack* new_fp_stack = new VanadisRegisterStack( fp_reg_count );

	for( uint16_t i = 0; i < fp_reg_count; ++i ) {
		if( ! retire_isa_tables[ hw_thr ]->physFPRegInUse( i ) ) {
			new_fp_stack->push(i);
		}
	}

	delete fp_register_stacks[hw_thr];
	fp_register_stacks[hw_thr] = new_fp_stack;

	output->verbose(CALL_INFO, 16, 0, "---> Floating point stack contains %" PRIu32 " registers.\n",
		(uint32_t) new_fp_stack->size());
}

void VanadisComponent::clearROBMisspeculate( const uint32_t hw_thr ) {
	VanadisCircularQueue<VanadisInstruction*>* rob_tmp = new
		VanadisCircularQueue<VanadisInstruction*>( rob[hw_thr]->capacity() );

	// Delete the old ROB since this is not clear and reset to an empty one
	delete rob[hw_thr];
	rob[hw_thr] = rob_tmp;
}

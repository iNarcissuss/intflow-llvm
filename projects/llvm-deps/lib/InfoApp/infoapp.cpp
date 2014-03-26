#include <sstream>
#include <fstream>
#include <string>
#include <vector>

#include "llvm/Instruction.h"
#include "llvm/Instructions.h"
#include "llvm/Function.h"
#include "llvm/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include "llvm/GlobalVariable.h"
#include "llvm/Constants.h"

#include "Infoflow.h"
#include "Slice.h"

#include "infoapp.h"
//#define __REACH__

//#define __DBG__
#define DBG_LINE 322
#define DBG_COL 23

using std::set;

using namespace llvm;
using namespace deps;

static void getWhiteList();

namespace {

static rmChecks *rmCheckList;

static const struct CallTaintEntry bLstSourceSummaries[] = {
	// function  tainted values   tainted direct memory tainted root ptrs
	{ "fgets",   TAINTS_RETURN_VAL,  TAINTS_ARG_1,      TAINTS_NOTHING },
	{ "getchar", TAINTS_RETURN_VAL,  TAINTS_NOTHING,    TAINTS_NOTHING },
	{ "_IO_getc",TAINTS_RETURN_VAL,  TAINTS_NOTHING,    TAINTS_NOTHING },
	{ 0,         TAINTS_NOTHING,     TAINTS_NOTHING,    TAINTS_NOTHING }
};

static const struct CallTaintEntry wLstSourceSummaries[] = {
	// function  tainted values   tainted direct memory tainted root ptrs
	{ "gettimeofday",   TAINTS_RETURN_VAL,  TAINTS_ARG_1,      TAINTS_NOTHING },
	{ 0,                TAINTS_NOTHING,     TAINTS_NOTHING,    TAINTS_NOTHING }
};

static const struct CallTaintEntry sensSinkSummaries[] = {
	// function  tainted values   tainted direct memory tainted root ptrs
	{ "malloc",   	TAINTS_ARG_1,  	TAINTS_NOTHING,    	TAINTS_NOTHING },
	{ "calloc",  TAINTS_ALL_ARGS,  	TAINTS_NOTHING,    	TAINTS_NOTHING },
	{ "realloc", TAINTS_ALL_ARGS,	TAINTS_NOTHING,    	TAINTS_NOTHING },
	{ 0,          TAINTS_NOTHING,	TAINTS_NOTHING,		TAINTS_NOTHING }
};

CallTaintEntry nothing = { 0, TAINTS_NOTHING, TAINTS_NOTHING, TAINTS_NOTHING };




/* ****************************************************************************
 * ============================================================================
 *  				    Pass Initialization & Fin Functions
 * ============================================================================
 * ****************************************************************************/

bool
InfoAppPass::runOnModule(Module &M)
{
	doInitializationAndRun(M);
	return false;
}

void
InfoAppPass::doInitializationAndRun(Module &M)
{
	unique_id = 0;
	infoflow = &getAnalysis<Infoflow>();
	getWhiteList();
	getMode();
	
	if (mode == WHITELISTING) {
		dbg_err("WhiteListing");
		runOnModuleWhitelisting(M);
	}
	else if (mode == BLACKLISTING){
		dbg_err("BlackListing");
		runOnModuleBlacklisting(M);
	}
	else if (mode == SENSITIVE) {
		dbg_err("Sensitive");
		runOnModuleSensitive(M);
	}
	else
		exit(mode);
	
	//doFinalization();
}

void
InfoAppPass::doFinalization() {
	dbg_err("doFinalizationWhitelisting");
	DenseMap<const Value*, bool>::const_iterator xi = xformMap.begin();
	DenseMap<const Value*, bool>::const_iterator xe = xformMap.end();

	for (;xi!=xe; xi++) {
		std::string output;
		raw_string_ostream rs(output);
		if (xi->second) {
			format_ioc_report_func(xi->first, rs);
			dbg_msg("[InfoApp]xformMap:", xi->second + ":" + rs.str());
		}
	}
	
	for (unsigned i=0; rmCheckList[i].func; i++) {
		delete rmCheckList[i].func;
		delete rmCheckList[i].fname;
	}
	
	delete rmCheckList;
}

/* ****************************************************************************
 * ============================================================================
 *  						Taint Functions
 * ============================================================================
 * ****************************************************************************/

//XXX: same function defined from SourceSinkAnalysis
static const CallTaintEntry *
findEntryForFunction(const CallTaintEntry *Summaries,
					 const std::string &FuncName) {
	unsigned Index;

	if (StringRef(FuncName).startswith("__ioc"))
		return &nothing;

	for (Index = 0; Summaries[Index].Name; ++Index) {
		if (Summaries[Index].Name == FuncName)
			return &Summaries[Index];
	}

	// Return the default summary.
	return &Summaries[Index];
}

void
InfoAppPass::taintForward(std::string srcKind,
						  CallInst *ci,
						  const CallTaintEntry *entry)
{
	const CallTaintSummary *vSum = &(entry->ValueSummary);
	const CallTaintSummary *dSum = &(entry->DirectPointerSummary);
	const CallTaintSummary *rSum = &(entry->RootPointerSummary);
	
	/* vsum */
	if (vSum->TaintsReturnValue)
		infoflow->setTainted(srcKind, *ci);

	for (unsigned ArgIndex = 0; ArgIndex < vSum->NumArguments; ++ArgIndex) {
		if (vSum->TaintsArgument[ArgIndex])
			infoflow->setTainted(srcKind, *(ci->getOperand(ArgIndex)));
	}

	/* dsum */
	if (dSum->TaintsReturnValue)
		infoflow->setDirectPtrTainted(srcKind, *ci);

	for (unsigned ArgIndex = 0; ArgIndex < dSum->NumArguments; ++ArgIndex) {
		if (dSum->TaintsArgument[ArgIndex])
			infoflow->setDirectPtrTainted(srcKind, *(ci->getOperand(ArgIndex)));
	}

	/* rsum */
	if (rSum->TaintsReturnValue)
		infoflow->setReachPtrTainted(srcKind, *ci);

	for (unsigned ArgIndex = 0; ArgIndex < rSum->NumArguments; ++ArgIndex) {
		if (rSum->TaintsArgument[ArgIndex])
			infoflow->setReachPtrTainted(srcKind, *(ci->getOperand(ArgIndex)));
	}
}

void
InfoAppPass::taintBackwards(std::string sinkKind,
							CallInst *ci,
							const CallTaintEntry *entry)
{
	const CallTaintSummary *vSum = &(entry->ValueSummary);
	const CallTaintSummary *dSum = &(entry->DirectPointerSummary);
	const CallTaintSummary *rSum = &(entry->RootPointerSummary);
	
	/* vsum */
	if (vSum->TaintsReturnValue)
		infoflow->setUntainted(sinkKind, *ci);

	for (unsigned ArgIndex = 0; ArgIndex < vSum->NumArguments; ++ArgIndex) {
		if (vSum->TaintsArgument[ArgIndex])
			infoflow->setUntainted(sinkKind, *(ci->getOperand(ArgIndex)));
	}

	/* dsum */
	if (dSum->TaintsReturnValue)
		infoflow->setDirectPtrUntainted(sinkKind, *ci);

	for (unsigned ArgIndex = 0; ArgIndex < dSum->NumArguments; ++ArgIndex) {
		if (dSum->TaintsArgument[ArgIndex])
			infoflow->setDirectPtrUntainted(sinkKind,
											*(ci->getOperand(ArgIndex)));
	}

	/* rsum */
	if (rSum->TaintsReturnValue)
		infoflow->setReachPtrUntainted(sinkKind, *ci);

	for (unsigned ArgIndex = 0; ArgIndex < rSum->NumArguments; ++ArgIndex) {
		if (rSum->TaintsArgument[ArgIndex])
			infoflow->setReachPtrUntainted(sinkKind,
										   *(ci->getOperand(ArgIndex)));
	}
}

bool
InfoAppPass::checkBackwardTainted(Value &V, InfoflowSolution* soln, bool direct)
{
	bool ret = (!soln->isTainted(V));

	if (direct) {
		ret = ret || (!soln->isDirectPtrTainted(V));
#ifdef __REACH__
		// XXX: not sure about Reachable pointer sets.
		ret = || (!soln->isReachPtrTainted(V));
#endif
	}

	return ret;
}

bool
InfoAppPass::checkForwardTainted(Value &V, InfoflowSolution* soln, bool direct)
{
	bool ret = (soln->isTainted(V));

	if (direct) {
		ret = ret || (soln->isDirectPtrTainted(V));
#ifdef __REACH__
		// XXX: not sure about Reachable pointer sets.
		ret = || (soln->isReachPtrTainted(V));
#endif
	}

	return ret;
}



/* ****************************************************************************
 * ============================================================================
 *  							IntFlow Modes
 * ============================================================================
 * ****************************************************************************/

void
InfoAppPass::runOnModuleWhitelisting(Module &M)
{
	for (Module::iterator mi = M.begin(); mi != M.end(); mi++) {
		Function& F = *mi;
		removeChecksForFunction(F, M);

		for (Function::iterator bi = F.begin(); bi != F.end(); bi++) {
			BasicBlock& B = *bi;
			for (BasicBlock::iterator ii = B.begin(); ii !=B.end(); ii++) {
				if (CallInst* ci = dyn_cast<CallInst>(ii)) {

					Function* func = ci->getCalledFunction();
					if (!func)
						continue;
					
					if (ioc_report_all_but_conv(func->getName())) {
#ifdef __DBG__
						uint32_t line = getIntFromVal(ci->getOperand(0));
						uint32_t col  = getIntFromVal(ci->getOperand(1));

						if (line != DBG_LINE || col != DBG_COL)
							continue;
#endif

						//check for arg. count
						assert(ci->getNumOperands() == 8);
						
						std::stringstream ss;
						ss << ci->getNumOperands();
						
						dbg_msg("numOper:", ss.str());
						dbg_msg("func_name:", func->getName());
						
						std::string sinkKind = getKindId("arithm",
														 &unique_id);
						InfoflowSolution* soln = 
							getBackSolArithm(sinkKind, ci);
						
						//check for simple const. assignment
						//getting valeMap
						std::set<const Value *> vMap;
						soln->getValueMap(vMap);

						if(isConstAssign(vMap)) {
							//replace it for simple const. assignment
							dbg_err("isConstAssign0:true");
							xformMap[ci] = true;
						} else {
							xformMap[ci] = trackSoln(M, soln, ci, sinkKind);
						}

						if (xformMap[ci]) {
							setWrapper(ci, M, func);
							//if (theofilos(M, ci) ) {
						}
					} else if (func->getName() == "__ioc_report_conversion") {
						//check for arg. count
						assert(ci->getNumOperands() == 10);

#ifdef __DBG__
						uint32_t line = getIntFromVal(ci->getOperand(0));
						uint32_t col  = getIntFromVal(ci->getOperand(1));

						if (line != DBG_LINE || col != DBG_COL)
							continue;
#endif

						std::string sinkKind = getKindId("conv",
														&unique_id);
						InfoflowSolution* soln = getBackSolConv(sinkKind, ci);

						//check for simple const. assignment
						std::set<const Value *> vMap;
						soln->getValueMap(vMap);

						if(isConstAssign(vMap))
						{
							//replace it for simple const. assignment
							dbg_err("isConstAssign1:true");
							xformMap[ci] = true;

						} else {
							xformMap[ci] = trackSoln(M, soln, ci, sinkKind);
						}

						if (xformMap[ci]) {
							//theofilos check 
							setWrapper(ci, M, func);
						}
					} else if ((func->getName() == "div")   ||
							   (func->getName() == "ldiv")  ||
							   (func->getName() == "lldiv") ||
							   (func->getName() == "iconv")
							  ) {
						setWrapper(ci, M, func);
					}
				}
			}
		}
	}
}

/* 
 * ===  FUNCTION  =============================================================
 *         Name:  runOnModuleBlacklisting
 *    Arguments:  @M - The source code module
 *  Description:  Implements InfoAppPass Blacklisting. Removes the checks from
 *  		  every operation unless this operation is identified as
 *  		  untrusted after forward (implemented here) and backward slicing
 *  		  (implemented in trackSinks).
 *  		  Untrusted sources are defined at bLstSourceSummaries.
 *  		  It also uses the whitelist provided at WHITE_LIST in order to
 *  		  remove manually identified benign operations.
 * ============================================================================
 */
void
InfoAppPass::runOnModuleBlacklisting(Module &M)
{
	Function *func;
	InfoflowSolution *fsoln;

	for (Module::iterator mi = M.begin(); mi != M.end(); mi++) {
		Function& F = *mi;
		//this is for whitelisting
		removeChecksForFunction(F, M);
		for (Function::iterator bi = F.begin(); bi != F.end(); bi++) {
			BasicBlock& B = *bi;
			for (BasicBlock::iterator ii = B.begin(); ii !=B.end(); ii++) {

				if (CallInst* ci = dyn_cast<CallInst>(ii)) {
					func = ci->getCalledFunction();
					if (!func)
						continue;
					const CallTaintEntry *entry =
						findEntryForFunction(bLstSourceSummaries,
											 func->getName());
					if (entry->Name) {
#ifdef __DBG__
						uint32_t line = getIntFromVal(ci->getOperand(0));
						uint32_t col = getIntFromVal(ci->getOperand(1));
						if (line != DBG_LINE || col != DBG_COL)
							continue;
#endif
						std::string srcKind = getKindId("src", &unique_id);
						fsoln = getForwardSolFromEntry(srcKind, ci, entry);

						backwardSlicingBlacklisting(M, fsoln, ci);
					}  else if ((func->getName() == "div")   ||
								(func->getName() == "ldiv")  ||
								(func->getName() == "lldiv") ||
								(func->getName() == "iconv")) {
						/* these need to be handled anyway */
						setWrapper(ci, M, func);
					}
				}
			} /* for-loops close here*/
		}
	}
	/* now xformMap holds all the information  */
	removeBenignChecks(M);
}

void
InfoAppPass::backwardSlicingBlacklisting(Module &M,
										InfoflowSolution* fsoln,
										CallInst* srcCI)
{
	Function *func;
	for (Module::iterator mi = M.begin(); mi != M.end(); mi++) {
		Function& F = *mi;
		dbg_msg("DBG0:fname:", F.getName());
		for (Function::iterator bi = F.begin(); bi != F.end(); bi++) {
			BasicBlock& B = *bi;
			for (BasicBlock::iterator ii = B.begin(); ii !=B.end(); ii++) {
				if (CallInst* ci = dyn_cast<CallInst>(ii)) {
					if (xformMap[ci])
						continue;
					
					func = ci->getCalledFunction();
					if (!func)
						continue;
					if (func->getName() == "__ioc_report_conversion") {
						xformMap[ci] = false;
						
						// this checks if it is tainted
						if (checkForwardTainted(*(ci->getOperand(7)), fsoln)) {
							//check for arg. count
							assert(ci->getNumOperands() == 10);
							
							//this returns all sources that are tainted
							std::string sinkKind   = getKindId("conv",
															   &unique_id);

							InfoflowSolution *soln = getBackSolConv(sinkKind,
																	ci);

							//check if source is in our list
							if (checkBackwardTainted(*srcCI, soln))
								xformMap[ci] = true;
						}
					
					} else if (ioc_report_all_but_conv(func->getName())) {
						xformMap[ci] = false;
						if (checkForwardTainted(*(ci->getOperand(4)), fsoln) ||
							checkForwardTainted(*(ci->getOperand(5)), fsoln)) {
							
							std::string sinkKind   = getKindId("arithm",
															   &unique_id);
							InfoflowSolution *soln =
								getBackSolArithm(sinkKind, ci);
							
							/* check if srcCI is backward tainted */
							if (checkBackwardTainted(*srcCI, soln))
								xformMap[ci] = true;
						}
					}
				}
			}
		}
	}
}

/* 
 * ===  FUNCTION  =============================================================
 *         Name:  runOnModuleSensitive
 *    Arguments:  @M - The source code module
 *  Description:  Implements InfoAppPass For Sensitive Sinks.
 *  		      Removes the checks from every operation unless this 
 *  			  operation's result is used in a sensitive sink. Sinks are 
 *  			  identified after first implementing a forward analysis 
 *  			  and then a and backward slicing. 
 * ============================================================================
 */
void
InfoAppPass::runOnModuleSensitive(Module &M)
{
	//populate Maps before doing anything else
	populateMapsSensitive(M);
	createArraysAndSensChecks(M);
	insertIOCChecks(M);

	Function *func;
	for (Module::iterator mi = M.begin(); mi != M.end(); mi++) {
		Function& F = *mi;
		removeChecksForFunction(F, M);
		for (Function::iterator bi = F.begin(); bi != F.end(); bi++) {
			BasicBlock& B = *bi;
			for (BasicBlock::iterator ii = B.begin(); ii !=B.end(); ii++) {

				if (CallInst* ci = dyn_cast<CallInst>(ii)) {

					func = ci->getCalledFunction();
					if (!func)
						continue;

					//remove all ioc_checks
					xformMap[ci] = false;
				}
			}
		}
	}

	removeBenignChecks(M);
}



/* ****************************************************************************
 * ============================================================================
 *  					   Sensitive Helper Functions
 * ============================================================================
 * ****************************************************************************/

GlobalVariable* 
InfoAppPass::createGlobalArray(Module &M, uint64_t size, std::string sinkKind)
{
	ArrayType* intPtr = ArrayType::get(IntegerType::get(M.getContext(), 32),
									   size);

	GlobalVariable* iocArray = 
		new GlobalVariable(/*Module=*/M, 
						   /*Type=*/intPtr,
						   /*isConstant=*/false,
						   /*Linkage=*/GlobalValue::ExternalLinkage,
						   /*Initializer=*/0, 
						   /*Name=*/"__gl_ioc_malloc_" + sinkKind);
	iocArray->setAlignment(8);
	
	/* Set Initializer */
	std::vector<Constant*> Initializer;
	Initializer.reserve(size);
	Constant* zero = ConstantInt::get(IntegerType::get(M.getContext(), 32), 0);

	for (uint64_t i = 0; i < size; i++) {
		Initializer[i++] = zero; 
	}

	ArrayType *ATy = ArrayType::get(IntegerType::get(M.getContext(), 32), size);
	Constant *initAr = llvm::ConstantArray::get(ATy, Initializer); 
	
	/* Initialize Array */
	iocArray->setInitializer(initAr);
	return iocArray;
}

GlobalVariable* 
InfoAppPass::getGlobalArray(Module &M, std::string sinkKind)
{
	iplist<GlobalVariable>::iterator gvIt;
	GlobalVariable *tmpgl = NULL;
	
	for (gvIt = M.global_begin(); gvIt != M.global_end(); gvIt++)
		if (gvIt->getName().str() == "__gl_ioc_malloc_" + sinkKind)
			tmpgl = gvIt;

	return tmpgl;
}

void
InfoAppPass::insertIntFlowFunction(Module &M,
								   std::string name, 
								   Instruction *ci,
								   BasicBlock::iterator ii,
								   GlobalVariable *tmpgl,
								   uint64_t idx)
{
	Function *f;
	Instruction *iocCheck;

	/* Get address of global array */
	std::vector<Value*> ptr_arrayidx_indices;
	ConstantInt* c_int32 = ConstantInt::get(M.getContext(),
											APInt(64, StringRef("0"), 10));
	Value *array_idx = ConstantInt::get(Type::getInt32Ty(M.getContext()), 0);
	ptr_arrayidx_indices.push_back(c_int32);
	ptr_arrayidx_indices.push_back(array_idx);
	
	/* ptr_arrayidx_m is the array addr for tmpgl */
	GetElementPtrInst* ptr_arrayidx_m = 
		GetElementPtrInst::Create(tmpgl, ptr_arrayidx_indices, "test", ii);
	
	PointerType* arrayPtr = 
		PointerType::get(IntegerType::get(M.getContext(), 32), 0);
	
	
	
	/* Construct Function */
	Constant *fc = M.getOrInsertFunction(name, 
							/* type */   Type::getInt32Ty(M.getContext()), 
							/* arg1 */ 	 arrayPtr,
							/* arg2 */	 Type::getInt32Ty(M.getContext()),
						 /* Linkage */   GlobalValue::ExternalLinkage,
										 (Type *)0);
	
	/* Create Args */
	std::vector<Value *> fargs;
	/* Push global array to args */
	fargs.push_back(ptr_arrayidx_m);
	/* Push number of elements */
	fargs.push_back(ConstantInt::get(M.getContext(), APInt(32, idx)));       

	f = cast<Function>(fc);
	ArrayRef<Value *> functionArguments(fargs);
	iocCheck = CallInst::Create(f, functionArguments, "");
	
	/* Insert Function */
	ci->getParent()->getInstList().insert(ci, iocCheck);
}

/* 
 * ===  FUNCTION  =============================================================
 *         Name:  populateMapsSensitive
 *    Arguments:  @M - The source code module
 *  Description:  TODO
 * ============================================================================
 */
void
InfoAppPass::populateMapsSensitive(Module &M)
{

	Function *func;
	BasicBlock *bb;
	for (Module::iterator mi = M.begin(); mi != M.end(); mi++) {
		Function& F = *mi;
		for (Function::iterator bi = F.begin(); bi != F.end(); bi++) {
			BasicBlock& B = *bi;
			for (BasicBlock::iterator ii = B.begin(); ii !=B.end(); ii++) {

				if (CallInst* ci = dyn_cast<CallInst>(ii)) {

					func = ci->getCalledFunction();
					if (!func)
						continue;
					
					std::string iocKind = "";

					if (StringRef(func->getName()).startswith("__ioc")) {
						dbg_err("------------------------------------");
						//get unique id for this ioc
						iocKind = getKindCall(M, F, ci);
						//create empty vector for this sink
						sensPoints[iocKind] = std::vector<std::string>();
						dbg_msg("checking sens sinks for ", iocKind);
					}

					if (ioc_report_arithm(func->getName())) {
						/*
						 * Get the output of the llvm.{ssad, ssub, smul,
						 * uadd, usub, umul}
						 * function in the parent basic block
						 * and use this as the taint source for forward
						 * slicing.
						 */
						bb = ci->getParent()->getSinglePredecessor();
						if (bb == NULL) {
							/* 
							 * problem...
							 * we should probably use a bb iterator and
							 * use that in order to get the predecessor
							 */
							dbg_err("Could not get predecessor (arithm)");
							continue;
						}
						
						/* Get parent basic block instructions */
						BasicBlock& BP = *bb;
						for (BasicBlock::iterator pii = BP.begin();
							 pii != BP.end();
							 pii++) {
							if (CallInst *cinst = dyn_cast<CallInst>(pii)) {
								if (cinst->getCalledFunction()) {
									std::string cfname = 
										cinst->getCalledFunction()->getName();

									if (llvm_arithm(cfname)) {
										/*
										 * use this instruction as the taint 
										 * source search for sensitive sink 
										 * starting from cinst
										 */
										searchSensFromArithm(F, M,
															 iocKind, cinst);
										break;
									}
								}
							}
						}	
					
					} else if (ioc_report_shl(func->getName())) {
						/*
						 * get the first instruction in the next basic
						 * block and use this as the taint source. This
						 * should be used with __ioc_report_shl_strict and
						 * __ioc_report_shr_bitwidth. For shl_bitwidth
						 * go 2 basic blocks "higher" (parent of parent)
						 * than __ioc_report_shl_strict
						 * and check for __ioc_report_shl_bitwidth.
						 */
						

						//for the moment, lets always check the next basic
						//block. if strict get first command if bitwidth get
						//second
						//get next BB
						bi++;
						dbg_msg("found shift ", func->getName());
		
						if (func->getName() == "__ioc_report_shl_strict") {
							//pass first instruction of the next bb
							searchSensFromInst(F, M, iocKind, bi->front());
						}
						
						//get second command if bitwidth:
						if (func->getName() == "__ioc_report_shr_bitwidth") {
							dbg_err("bitwidth");
							/* Get next basic block's instructions */
							BasicBlock& SB = *bi;
							for (BasicBlock::iterator sii = SB.begin();
								 sii != SB.end();
								 sii++) {

								//search from second command of next BB
								sii++;
								searchSensFromInst(F, M, iocKind, *sii);
								break;
							}

							//restore BB
							bi--;
							continue;
						}
					} else if (func->getName() == "__ioc_report_conversion") {
						/*
						 * do what???
						 */
					} else if (func->getName() == "__ioc_report_div_error") {
						/*
						 * go to the next basic block and use the first
						 * instruction as the taint source.
						 */
						bi++;
						searchSensFromInst(F, M, iocKind, bi->front());
						bi--;
						continue;
					} else {
						/*
						 * do nothing. Left as a placeholder in case I
						 * missed something
						 */
					}
				}
			}
		}
	}
}

/* 
 * ===  FUNCTION  =============================================================
 *         Name:  createArraysAndSensChecks
 *    Arguments:  @M - The source code module
 *  Description:  Create global arrays and add checks for sens sinks
 * ============================================================================
 */
void
InfoAppPass::createArraysAndSensChecks(Module &M)
{
	Function *func;
	for (Module::iterator mi = M.begin(); mi != M.end(); mi++) {
		Function& F = *mi;
		for (Function::iterator bi = F.begin(); bi != F.end(); bi++) {
			BasicBlock& B = *bi;
			for (BasicBlock::iterator ii = B.begin(); ii !=B.end(); ii++) {

				if (CallInst* ci = dyn_cast<CallInst>(ii)) {

					func = ci->getCalledFunction();
					if (!func)
						continue;
					
					const CallTaintEntry *entry =
						findEntryForFunction(sensSinkSummaries,
											 func->getName());
					if (entry->Name) {
						std::string sinkKind = getKindCall(M, F, ci);
						
						dbg_msg("creating global array for ", sinkKind);
						//get all ioc checks that lead to this sink
						uint64_t totalIOC = iocPoints[sinkKind].size();

						//create the respective global array
						GlobalVariable *glA = createGlobalArray(M,
																totalIOC,
																sinkKind);

						//insert the check before the operation
						insertIntFlowFunction(M,
											  "checkIOC",
											  dyn_cast<Instruction>(ii),
											  ii, 
											  glA,
											  totalIOC);

					} 
				}
			} /* for-loops close here*/
		}
	}
}

/*
 * ===  FUNCTION  =============================================================
 *         Name:  searchSensFromInst
 *  Description:  Searches for sensitive sink starting from first instr
 *  			  of next BB
 *    Arguments:  @M - the source code module
 *    			  @ci - the llvm{sadd, ssub, smul, uadd, usub, umul} function
 *    			  call
 * ============================================================================
 */
void
InfoAppPass::searchSensFromInst(Function &F,
								   Module &M,
								   std::string iocKind,
								   Instruction &i)
{
	std::string srcKind = getKindInst(M, F, i);
	dbg_msg("called from ", iocKind);
	dbg_msg("searching sens sink affected by ", srcKind);

	// we examine sensitive sinks from the above block: get Forward Solution
	// The instruction IS the result so taint this
	infoflow->setTainted(srcKind, i);
	std::set<std::string> kinds;
	kinds.insert(srcKind);
	InfoflowSolution *fsoln = infoflow->leastSolution(kinds, false, true);
	
	backSensitiveInst(F, M, i, iocKind, fsoln);
}
/* -----  end of function searchSensitiveFromStrictSh  ----- */

/*
 * ===  FUNCTION  =============================================================
 *         Name:  backSensitiveInst
 *  Description:  Iterates over the instructions looking for sensitive
 *  			  functions that are forward tainted. It uses backward slicing
 *  			  to determine if forward slicing is accurate. Then it handles
 *  			  those that are actually correct
 *    Arguments:  @M - the source code module
 *    			  @srcCI - the arithmetic operation used for forward tainting
 *    			  @fsoln - the forward slicing solution
 * ============================================================================
 */
void
InfoAppPass::backSensitiveInst(Function &F,
							   Module &M,
							   Instruction &srcCI,
							   std::string iocKind, 
							   InfoflowSolution *fsoln)
{
	Function *func;

	for (Module::iterator mi = M.begin(); mi != M.end(); mi++) {
		Function& F = *mi;
		for (Function::iterator bi = F.begin(); bi != F.end(); bi++) {
			BasicBlock& B = *bi;
			for (BasicBlock::iterator ii = B.begin(); ii !=B.end(); ii++) {
				if (CallInst* ci = dyn_cast<CallInst>(ii)) {
					func = ci->getCalledFunction();
					if (!func)
						continue;

					const CallTaintEntry *entry =
						findEntryForFunction(sensSinkSummaries,
											 func->getName());
					if (entry->Name) {
						/* this is a sensitive function */
						
						/* TODO: need a way to handle different functions too
						 * (apart from malloc)
						 * FIXME: THE REST OF THE FUNCTION IS FOR MALLOC ONLY!
						 */

						if (checkForwardTainted(*(ci->getOperand(0)), fsoln )) {
							/* backward slicing needed */
							std::string sinkKind = getKindCall(M, F, ci);
							infoflow->setUntainted(sinkKind,
												   *(ci->getOperand(0)));
							
							std::set<std::string> kinds;
							kinds.insert(sinkKind);
							
							InfoflowSolution *soln =
								infoflow->greatestSolution(kinds, false);
							
							if (checkBackwardTainted(srcCI, soln)) {
								//TODO add a check here to skip the for loop
								handleStrictShift(iocKind, sinkKind, F, M);
								
								//add sens sink to this ioc
								sensPoints[iocKind].push_back(sinkKind);
								dbg_err("*************************");
								dbg_msg("sink : ", sinkKind);
								dbg_msg("adding ioc: ", iocKind);
								//and add ioc the sens sink list
								iocPoints[sinkKind].push_back(iocKind);
									/* this one needs to be handled */
									/* TODO: How to handle these cases? */

								dbg_msg("found sensitive: ", sinkKind);
							}
						}
					}
				}
			}
		}
	}
}
/* -----  end of function backSensitiveInst  ----- */


/*
 * ===  FUNCTION  =============================================================
 *         Name:  handleStrictShift
 *  Description:  Checks for shl bitwidth if we have shl_strict
 *    Arguments:  @B - shl_strict Basic Block
 *    			  TODO
 * ============================================================================
 */
void
InfoAppPass::handleStrictShift(std::string iocKind,
							   std::string sinkKind,
							   Function &F,
							   Module &M)
{
	std::string shlKind;
	Function *func;
	Function *pfunc;
	bool found_shl = false;

	for (Function::iterator bi = F.begin(); bi != F.end(); bi++) {
		BasicBlock& B = *bi;
		for (BasicBlock::iterator ii = B.begin(); ii != B.end(); ii++) {
			if (CallInst* ci = dyn_cast<CallInst>(ii)) {
				func = ci->getCalledFunction();
				if (!func)
					continue;
				if (func->getName() == "__ioc_report_shl_strict") {
					//go two bb up
					bi--;
					bi--;

					BasicBlock& PB = *bi;
					for (BasicBlock::iterator pii = PB.begin();
						 pii != PB.end();
						 pii++) {

						if (CallInst* pci = dyn_cast<CallInst>(pii)) {
							pfunc = pci->getCalledFunction();
							if (!pfunc)
								continue;

							std::string pfname = pfunc->getName();
							if (pfname == "__ioc_report_shl_bitwidth") {
								shlKind = getKindCall(M, F, pci);
								found_shl = true;
								break;
							}
						}
					}
				}
			}
			if (found_shl)
				break;
		}
		if (found_shl)
			break;
	}
	
	if (found_shl) {
		sensPoints[shlKind].push_back(sinkKind);
		dbg_err("*************************");
		dbg_msg("sink : ", sinkKind);
		dbg_msg("adding ioc: ", shlKind);
		//and add ioc the sens sink list
		iocPoints[sinkKind].push_back(shlKind);
		dbg_msg("found sensitive: ", sinkKind);
	}

}

/*
 * ===  FUNCTION  =============================================================
 *         Name:  searchSensFromArithm
 *  Description:  Searches for sensitive sink starting from arithmetic
 *  			  operations.
 *    Arguments:  @M - the source code module
 *    			  @ci - the llvm{sadd, ssub, smul, uadd, usub, umul} function
 *    			  call
 * ============================================================================
 */
void
InfoAppPass::searchSensFromArithm(Function &F,
								   Module &M,
								   std::string iocKind,
								   CallInst *ci)
{
	std::string srcKind = getKindCall(M, F, ci);
	dbg_msg("called from ", iocKind);
	dbg_msg("searching sens sink affected by ", srcKind);

	// we examine sensitive sinks from the above block
	InfoflowSolution *fsoln = getForwardSol(srcKind, ci);

	/*
	 * iterate the instructions and check if there are tainted 
	 * sensitive sinks if true, backward slice and check for ci.
	 */
	backSensitiveArithm(M, ci, iocKind, fsoln);
}
/* -----  end of function searchSensitive  ----- */


/*
 * ===  FUNCTION  =============================================================
 *         Name:  backSensitiveArithm
 *  Description:  Iterates over the instructions looking for sensitive
 *  			  functions that are forward tainted. It uses backward slicing
 *  			  to determine if forward slicing is accurate. Then it handles
 *  			  those that are actually correct
 *    Arguments:  @M - the source code module
 *    			  @srcCI - the arithmetic operation used for forward tainting
 *    			  @fsoln - the forward slicing solution
 * ============================================================================
 */
void
InfoAppPass::backSensitiveArithm(Module &M,
								 CallInst *srcCI,
								 std::string iocKind, 
								 InfoflowSolution *fsoln)
{
	Function *func;

	for (Module::iterator mi = M.begin(); mi != M.end(); mi++) {
		Function& F = *mi;
		for (Function::iterator bi = F.begin(); bi != F.end(); bi++) {
			BasicBlock& B = *bi;
			for (BasicBlock::iterator ii = B.begin(); ii !=B.end(); ii++) {
				if (CallInst* ci = dyn_cast<CallInst>(ii)) {
					func = ci->getCalledFunction();
					if (!func)
						continue;

					const CallTaintEntry *entry =
						findEntryForFunction(sensSinkSummaries,
											 func->getName());
					if (entry->Name) {
						/* this is a sensitive function */
						
						/* TODO: need a way to handle different functions too
						 * (apart from malloc)
						 * FIXME: THE REST OF THE FUNCTION IS FOR MALLOC ONLY!
						 */

						if (checkForwardTainted(*(ci->getOperand(0)), fsoln )) {
							/* backward slicing needed */
							std::string sinkKind = getKindCall(M, F, ci);
							infoflow->setUntainted(sinkKind,
												   *(ci->getOperand(0)));
							
							std::set<std::string> kinds;
							kinds.insert(sinkKind);
							
							InfoflowSolution *soln =
								infoflow->greatestSolution(kinds, false);
							
							/*
							 * FIXME: need to check: do all llvm{sadd, etc}
							 * have the same two arguments in the same spots?
							 */
							if (checkBackwardTainted(*(srcCI->getOperand(0)),
													 soln) ||
								checkBackwardTainted(*(srcCI->getOperand(1)),
													   soln)) {
								
								//add sens sink to this ioc
								sensPoints[iocKind].push_back(sinkKind);
								dbg_err("*************************");
								dbg_msg("sink : ", sinkKind);
								dbg_msg("adding ioc: ", iocKind);
								//and add ioc the sens sink list
								iocPoints[sinkKind].push_back(iocKind);
									/* this one needs to be handled */
									/* TODO: How to handle these cases? */

								dbg_msg("found sensitive: ", sinkKind);
							}
						}
					}
				}
			}
		}
	}
}
/* -----  end of function backSensitiveArithm  ----- */

/* 
 * ===  FUNCTION  =============================================================
 *         Name:  insertIOCChecks
 *    Arguments:  @M - The source code module
 *  Description:  TODO
 * ============================================================================
 */
void
InfoAppPass::insertIOCChecks(Module &M)
{

	GlobalVariable *glA = NULL;
	Function *func;
	BasicBlock *bb;

	uint64_t glA_pos = 0;

	for (Module::iterator mi = M.begin(); mi != M.end(); mi++) {
		Function& F = *mi;
		for (Function::iterator bi = F.begin(); bi != F.end(); bi++) {
			BasicBlock& B = *bi;
			for (BasicBlock::iterator ii = B.begin(); ii !=B.end(); ii++) {

				if (CallInst* ci = dyn_cast<CallInst>(ii)) {

					func = ci->getCalledFunction();
					if (!func)
						continue;

					std::string fname = func->getName();
					std::string iocKind = "";

					if (StringRef(fname).startswith("__ioc")) {

						//get unique id for this ioc
						iocKind = getKindCall(M, F, ci);

						//see how many sensitive sinks we have for iocKind
						if (sensPoints[iocKind].empty())
							continue;

						sensPointVector spv = sensPoints[iocKind];

						// Insert one check for each malloc
						for(sensPointVector::const_iterator svi =
							spv.begin();
							svi != spv.end();
							++svi) {

							std::string sink = *svi;

							//get position of this IOC in the sens sink array
							iocPointVector ipv = iocPoints[sink];
							glA_pos = find(ipv.begin(), ipv.end(), iocKind) - 
								ipv.begin();

							//create the respective global array
							glA = getGlobalArray(M, sink);

							//insert the check before the operation
							insertIntFlowFunction(M,
												  "setTrueIOC",
												  dyn_cast<Instruction>(ii),
												  ii,
												  glA,
												  glA_pos); 

							//Now we definitely have a sens sink related 
							//with this IOC, on to add the setFalseIOC,
							if (ioc_report_arithm(fname) 				||
								ioc_report_shl(fname)  					||
								(fname == "__ioc_report_conversion")  	||
								(fname == "__ioc_report_div_error")) {

								bb = ci->getParent()->getSinglePredecessor();
								if (bb == NULL) {
									dbg_err("Could not get predecessor");
									continue;
								}

								/* Get parent basic block instructions */
								BasicBlock& BP = *bb;
								if (!bb)
									continue;
								dbg_msg("setting False in ", bb->getName());
								for (BasicBlock::iterator pii = BP.begin();
									 pii != BP.end();
									 pii++) {
									
									Instruction *pinst = 
										dyn_cast<Instruction>(pii);

									insertIntFlowFunction(M,
														  "setFalseIOC",
														  pinst,
														  pii,
														  glA,
														  glA_pos);
									break;
								}
							}
						}
					}
				}
			}
		}
	}
}

/* ****************************************************************************
 * ============================================================================
 *  					Functions Removing IOC stuff
 * ============================================================================
 * ****************************************************************************/

/* 
 * ===  FUNCTION  =============================================================
 *         Name:  removeBenignChecks
 *    Arguments:  @M - the source code module
 *  Description:  Iterates over the module and checks how every call instruction
 *  			  If this instruction is noted as trusted (xformMap has a false
 *  			  value), then we remove the checks.
 * ============================================================================
 */
void
InfoAppPass::removeBenignChecks(Module &M)
{
	for (Module::iterator mi = M.begin(); mi != M.end(); mi++) {
		Function& F = *mi;
		for (Function::iterator bi = F.begin(); bi != F.end(); bi++) {
			BasicBlock& B = *bi;
			for (BasicBlock::iterator ii = B.begin(); ii !=B.end(); ii++) {
				if (CallInst* ci = dyn_cast<CallInst>(ii)) {
					Function *func = ci->getCalledFunction();
					if (!func)
						continue;
					if (ioc_report_all(func->getName()) && !xformMap[ci] ){
						setWrapper(ci, M, func);
					}
				}
			}
		}
	}
}

void
InfoAppPass::removeChecksForFunction(Function& F, Module& M) {
	for (unsigned i=0; rmCheckList[i].func; i++) {
		if (F.getName() == rmCheckList[i].func) {
			for (Function::iterator bi = F.begin(); bi != F.end(); bi++) {
				BasicBlock& B = *bi;
				for (BasicBlock::iterator ii = B.begin(); ii !=B.end(); ii++) {
					if (CallInst* ci = dyn_cast<CallInst>(ii))
						removeChecksInst(ci, i, M);
				}
			}
		}
	}
}

void
InfoAppPass::removeChecksInst(CallInst *ci, unsigned int i, Module &M)
{
	Function* f = ci->getCalledFunction();
	if (!f)
		return;
	
	std::string fname = f->getName();
	
	if (
		/* remove overflow */
		(rmCheckList[i].overflow && ioc_report_arithm(fname)) 				||
		/* remove conversion */
		(rmCheckList[i].conversion && (fname == "__ioc_report_conversion")) ||
		/* remove shift */
		(rmCheckList[i].shift && ioc_report_shl(fname))) {
			xformMap[ci] = true;
			setWrapper(ci, M, f);
	}
}

/* ****************************************************************************
 * ============================================================================
 *  						Solution Functions
 * ============================================================================
 * ****************************************************************************/

InfoflowSolution *
InfoAppPass::getForwardSolFromEntry(std::string srcKind,
								 CallInst *ci,
								 const CallTaintEntry *entry)
{
	
	//XXX Do not change order
	taintForward(srcKind, ci, entry);
	
	std::set<std::string> kinds;
	kinds.insert(srcKind);
	
	//This does forward analysis
	InfoflowSolution *fsoln = infoflow->leastSolution(kinds, false, true);

	return fsoln;
}

InfoflowSolution *
InfoAppPass::getBackwardsSolFromEntry(std::string sinkKind,
							 CallInst *ci,
							 const CallTaintEntry *entry)
{
	
	//XXX Do not change order
	taintBackwards(sinkKind, ci, entry);
	
	std::set<std::string> kinds;
	kinds.insert(sinkKind);
	
	InfoflowSolution *fsoln = infoflow->greatestSolution(kinds, false);

	return fsoln;
}

InfoflowSolution *
InfoAppPass::getForwardSol(std::string srcKind, CallInst *ci)
{
	
	//FIXME taint Direct pointers as well?
	infoflow->setTainted(srcKind, *ci);
	
	std::set<std::string> kinds;
	kinds.insert(srcKind);
	InfoflowSolution *fsoln = infoflow->leastSolution(kinds, false, true);
	
	return fsoln;
}

InfoflowSolution *
InfoAppPass::getBackwardsSol(std::string sinkKind, CallInst *ci)
{
	
	//XXX Do not change order
	infoflow->setUntainted(sinkKind, *ci);
	
	std::set<std::string> kinds;
	kinds.insert(sinkKind);
	
	InfoflowSolution *fsoln = infoflow->greatestSolution(kinds, false);

	return fsoln;
}

InfoflowSolution *
InfoAppPass::getForwSolArithm(std::string srcKind, CallInst *ci)
{
	Value* lval = ci->getOperand(4);
	Value* rval = ci->getOperand(5);

	//tagging lVal
	infoflow->setTainted(srcKind, *lval);
	
	//tagging rVal
	infoflow->setTainted(srcKind, *rval);

	std::set<std::string> kinds;
	kinds.insert(srcKind);
	
	return infoflow->leastSolution(kinds, false, true);
}

InfoflowSolution *
InfoAppPass::getBackSolArithm(std::string sinkKind, CallInst *ci)
{
	Value* lval = ci->getOperand(4);
	Value* rval = ci->getOperand(5);

	//tagging lVal
	infoflow->setUntainted(sinkKind, *lval);
	
	//tagging rVal
	infoflow->setUntainted(sinkKind, *rval);

	std::set<std::string> kinds;
	kinds.insert(sinkKind);
	
	return infoflow->greatestSolution(kinds, false);
}

InfoflowSolution *
InfoAppPass::getBackSolConv(std::string sinkKind, CallInst *ci)
{
	Value* val = ci->getOperand(7);
	infoflow->setUntainted(sinkKind, *val);
	
	std::set<std::string> kinds;
	kinds.insert(sinkKind);
	
	return infoflow->greatestSolution(kinds, false);
}

InfoflowSolution *
InfoAppPass::getForwSolConv(std::string srcKind, CallInst *ci)
{
	Value* val = ci->getOperand(7);
	infoflow->setTainted(srcKind, *val);
	
	std::set<std::string> kinds;
	kinds.insert(srcKind);
	
	return infoflow->leastSolution(kinds, false, true);
}


/* 
 * ===  FUNCTION  =============================================================
 *         Name:  trackSoln
 *    Arguments:  @M - the source code module
 *  Description:  TODO
 *  		
 * ============================================================================
 */
bool
InfoAppPass::trackSoln(Module &M,
		InfoflowSolution* soln,
		CallInst* sinkCI,
		std::string& kind)
{
	dbg_err("trackSoln");
	//by default do not change/replace.
	bool ret = false;
	
	//need optimization or parallelization
	for (Module::iterator mi = M.begin(); mi != M.end(); mi++) {
		Function& F = *mi;
		
		for (Function::iterator bi = F.begin(); bi != F.end(); bi++) {
			BasicBlock& B = *bi;
			for (BasicBlock::iterator ii = B.begin(); ii !=B.end(); ii++) {
				if (checkBackwardTainted(*ii, soln)) {
					DEBUG(ii->dump());
					if (CallInst* ci = dyn_cast<CallInst>(ii)) {
						ret = ret || trackSolnInst(ci, M, sinkCI, soln, kind);
					}
				}
			}
		}
	}
	return ret;
}




/* FIXME refactor */
/* 
 * ===  FUNCTION  =============================================================
 *         Name:  trackSolnInst
 *    Arguments:  @M - the source code module
 *    			TODO
 *  Description: checks first if each instruction in the solution is forward
 *  			tainted and returns true or false
 *  		
 * ============================================================================
 */
bool 
InfoAppPass::trackSolnInst(CallInst *ci,
						   Module &M,
						   CallInst* sinkCI,
						   InfoflowSolution* soln,
						   std::string& kind)
{
	bool ret = false;
	
	Function* func = ci->getCalledFunction();
	if (!func)
		return false;
	std::string fname = func->getName();

	//check for white-listing
	const CallTaintEntry *entry = findEntryForFunction(wLstSourceSummaries,
													   fname);
	if (entry->Name && mode == WHITELISTING) {
		dbg_msg("white-list:", fname);
		std::string srcKind = "src0" + kind;

		InfoflowSolution* fsoln = getForwardSolFromEntry(srcKind, ci, entry);
		
		Function* sinkFunc = sinkCI->getCalledFunction();
		if(!sinkFunc)
			return false;
		if(sinkFunc->getName() == "__ioc_report_conversion")
		{
			if (checkForwardTainted(*(sinkCI->getOperand(7)), fsoln)) {
				dbg_err("checkForwardTainted:white0:true");
				ret = true;
			} else {
				dbg_err("checkForwardTainted:white0:false");
				ret = false;
			}

		} else if (ioc_report_all_but_conv(sinkFunc->getName()))
		{
			if (checkForwardTainted(*(sinkCI->getOperand(4)), fsoln) ||
				checkForwardTainted(*(sinkCI->getOperand(5)), fsoln)) {
				dbg_err("checkForwardTainted:white1:true");
				ret = true;
			} else {
				dbg_err("checkForwardTainted:white1:false");
				ret = false;
			}

		} else {
			assert(false && "not __ioc_report function");
		}
	}

	entry = findEntryForFunction(bLstSourceSummaries, fname);

	if (entry->Name && mode == BLACKLISTING) {
		dbg_msg("black-list: ", fname);
		std::string srcKind = "src1" + kind;
		InfoflowSolution* fsoln = getForwardSolFromEntry(srcKind, ci, entry);
		
		Function* sinkFunc = sinkCI->getCalledFunction();
		if(!sinkFunc)
			return false;
		if(sinkFunc->getName() == "__ioc_report_conversion") {
			if (checkForwardTainted(*(sinkCI->getOperand(7)), fsoln)) {
				dbg_err("checkForwardTainted:black0:true");
				//tainted source detected! just get out
				return false;
			} else {
				dbg_err("checkForwardTainted:black0:false");
			}

		} else if (ioc_report_all_but_conv(sinkFunc->getName()))
		{
			if (checkForwardTainted(*(sinkCI->getOperand(4)), fsoln) ||
				checkForwardTainted(*(sinkCI->getOperand(5)), fsoln))
			{
				dbg_err("checkForwardTainted:black1:true");
				return false;
			} else {
				dbg_err("checkForwardTainted:black1:false");
			}
		} else {
			assert(false && "not __ioc_report function");
		}
	}

	if (mode == SENSITIVE) {
		dbg_msg("backward led to ", fname);
	}
	
	return ret;
}

/* ****************************************************************************
 * ============================================================================
 *  								CHECKS
 * ============================================================================
 * ****************************************************************************/

bool
InfoAppPass::isConstAssign(const std::set<const Value *> vMap) {
	std::set<const Value *>::const_iterator vi = vMap.begin();
	std::set<const Value *>::const_iterator ve = vMap.end();

	for (;vi!=ve; vi++) {
		const Value* val = (const Value*) *vi;
		if (const CallInst* ci = dyn_cast<const CallInst>(val)) {
			Function* func = ci->getCalledFunction();
			//assert(func && "func should be fine!");
			if (func && func->getName().startswith("llvm.ssub.with.overflow")) {
				continue;
			} else {
				//XXX: need more for other function calls
				dbg_msg("isConstAssign:", func->getName());
				return false;
			}
		} else if (dyn_cast<const LoadInst>(val)) {
			return false;
		} else {
			//XXX: need more for other instructions
		}
	}
	return true;
}

bool
InfoAppPass::ioc_report_all_but_conv(std::string name)
{
	return (name == "__ioc_report_add_overflow" ||
			name == "__ioc_report_sub_overflow" ||
			name == "__ioc_report_mul_overflow" ||
			name == "__ioc_report_shr_bitwidth" ||
			name == "__ioc_report_shl_bitwidth" ||
			name == "__ioc_report_shl_strict");
}

bool
InfoAppPass::ioc_report_all(std::string name)
{
	return (name == "__ioc_report_add_overflow" ||
			name == "__ioc_report_sub_overflow" ||
			name == "__ioc_report_mul_overflow" ||
			name == "__ioc_report_shr_bitwidth" ||
			name == "__ioc_report_shl_bitwidth" ||
			name == "__ioc_report_shl_strict"	||
			name == "__ioc_report_conversion");
}

bool
InfoAppPass::ioc_report_arithm(std::string name)
{
	return (name == "__ioc_report_add_overflow" ||
			name == "__ioc_report_sub_overflow" ||
			name == "__ioc_report_mul_overflow");
}

bool
InfoAppPass::ioc_report_shl(std::string name)
{
	return (name == "__ioc_report_shr_bitwidth" ||
			name == "__ioc_report_shl_bitwidth" ||
			name == "__ioc_report_shl_strict");
}

/* FIXME should we add llvm.fmuladd.* */
bool
InfoAppPass::llvm_arithm(std::string name)
{
	return (StringRef(name).startswith("llvm.sadd.with.overflow") ||
			StringRef(name).startswith("llvm.uadd.with.overflow") ||
			StringRef(name).startswith("llvm.ssub.with.overflow") ||
			StringRef(name).startswith("llvm.usub.with.overflow") ||
			StringRef(name).startswith("llvm.smul.with.overflow") ||
			StringRef(name).startswith("llvm.umul.with.overflow"));
}

/* ****************************************************************************
 * ============================================================================
 *  							HELPER FUNCTIONS 
 * ============================================================================
 * ****************************************************************************/

uint64_t
InfoAppPass::getIntFromVal(Value* val)
{
	ConstantInt* num = dyn_cast<ConstantInt>(val);
	assert(num && "constant int casting check");
	return num->getZExtValue();
}

void
InfoAppPass::getStringFromVal(Value* val, std::string& output)
{
	Constant* gep = dyn_cast<Constant>(val);
	assert(gep && "assertion");
	GlobalVariable* global = dyn_cast<GlobalVariable>(gep->getOperand(0));
	assert(global && "assertion");
	ConstantDataArray* array =
		dyn_cast<ConstantDataArray>(global->getInitializer());
	if (array->isCString())
		output = array->getAsCString();
}

/*
 * Helper Functions
 */

std::string
InfoAppPass::getKindInst(Module &M, Function &F, Instruction &ci)
{
	std::stringstream SS;
	
	//Get function name that contains the CallInst
	std::string tmp = M.getModuleIdentifier();
	SS << tmp;
	SS << ":";
	
	tmp = F.getName();
	SS << tmp;
	SS << ":";
	
	//get called function
	tmp = ci.getOpcodeName();
	SS << tmp;
	SS << ":";

	//get label inside bb
	if (ci.getParent())
		tmp = ci.getParent()->getName();
	SS << tmp;
	
	std::string stringKind = SS.str();
	return stringKind;
}

std::string
InfoAppPass::getKindCall(Module &M, Function &F, CallInst *ci)
{
	std::stringstream SS;
	//Get function name that contains the CallInst
	std::string tmp = M.getModuleIdentifier();
	SS << tmp;
	SS << ":";
	
	tmp = F.getName();
	SS << tmp;
	SS << ":";
	
	//get called function
	Function *func = ci->getCalledFunction();
	if (func)
		tmp = func->getName();
	else
		tmp = "main";
	SS << tmp;
	SS << ":";

	//get label inside bb
	if (ci->getParent())
		tmp = ci->getParent()->getName();
	SS << tmp;
	
	std::string stringKind = SS.str();
	return stringKind;
}

std::string
InfoAppPass::getKindId(std::string name, uint64_t *unique_id)
{
	std::stringstream SS;
	SS << (*unique_id)++;
	return name + SS.str();
}

/* 
 * ===  FUNCTION  =============================================================
 *         Name:  getMode
 *    Arguments:  -
 *  Description:  Read the mode from the file @MODE_FILE.
 * ============================================================================
 */
void
InfoAppPass::getMode() {
	std::ifstream ifmode;
	std::string tmp;
	ifmode.open(MODE_FILE);
	if (!ifmode.is_open()) {
		dbg_err("Failed to open mode file");
		exit(1);
	}
	ifmode >> tmp;
	dbg_msg("mode is:", tmp);
	mode = (unsigned char) atoi(tmp.c_str());
	ifmode >> tmp;
	if (ifmode.good()) {
		dbg_err("Mode File contains more than 1 number");
		exit(1);
	}
	if (mode < 1 || mode > MODE_MAX_NUM) {
		dbg_err("Wrong mode number");
		exit(1);
	}
}

}  //namespace deps

namespace  {
/* ID for InfoAppPass */
char InfoAppPass::ID = 0;

static RegisterPass<InfoAppPass>
	XX ("infoapp", "implements infoapp", true, true);


static void initializeInfoAppPasses(PassRegistry &Registry) {
	llvm::initializeAllocIdentifyPass(Registry);
	llvm::initializePDTCachePass(Registry);
}

static void registerInfoAppPasses(const PassManagerBuilder &,
								  PassManagerBase &PM)
{
	PM.add(llvm::createPromoteMemoryToRegisterPass());
	PM.add(llvm::createPDTCachePass());
	PM.add(new InfoAppPass());
}

class StaticInitializer {
	public:
		StaticInitializer() {
			char* passend = getenv("__PASSEND__");

			if (passend) {
				dbg_err("== EP_LoopOptimizerEnd ==");
				RegisterStandardPasses
				RegisterInfoAppPass(PassManagerBuilder::EP_LoopOptimizerEnd,
							registerInfoAppPasses);
			} else {
				dbg_err("== EP_ModuleOptimizerEarly ==");
				RegisterStandardPasses
				RegisterInfoAppPass(PassManagerBuilder::EP_ModuleOptimizerEarly,
							registerInfoAppPasses);
			}

			PassRegistry &Registry = *PassRegistry::getPassRegistry();
			initializeInfoAppPasses(Registry);
		}
};

static StaticInitializer InitializeEverything;


void
dbg_err(std::string s)
{
	llvm::errs() << "[InfoApp] DBG:" << s << "\n";
}

void
dbg_msg(std::string a, std::string b)
{
	llvm::errs() << "[InfoApp] DBG:" << a << b << "\n";
}

} /* end of namespace */


static void
getWhiteList() {
	std::string line, file, function, conv;
	std::string overflow, shift;
	bool conv_bool, overflow_bool, shift_bool;
	unsigned numLines;
	unsigned i;
	unsigned pos = 0;
	std::ifstream whitelistFile;
	whitelistFile.open(WHITE_LIST);
	//get number of lines
	numLines = 0;
	while (whitelistFile.good()) {
		std::getline(whitelistFile, line);
		if (!line.empty())
			numLines++;
	}

	whitelistFile.clear();
	whitelistFile.seekg(0, std::ios::beg);

	rmCheckList = new rmChecks[numLines];
	for (i = 0; i < numLines; i++) {
		getline(whitelistFile, line);
		//handle each line
		pos = 0;
		function = line.substr(pos, line.find(","));
		pos = line.find(",") + 1;
		file = line.substr(pos, line.find(",", pos) - pos);
		pos = line.find(",", pos) + 1;
		conv = line.substr(pos, line.find(",", pos) - pos);
		pos = line.find(",", pos) + 1;
		overflow = line.substr(pos, line.find(",", pos) - pos);
		pos = line.find(",", pos) + 1;
		shift = line.substr(pos, line.size() - pos);

		if (conv.compare("true") == 0)
			conv_bool = true;
		else
			conv_bool = false;

		if (overflow.compare("true") == 0)
			overflow_bool = true;
		else
			overflow_bool = false;

		if (shift.compare("true") == 0)
			shift_bool = true;
		else
			shift_bool = false;

		if (function.compare("0") == 0)
			rmCheckList[i].func = (char*) 0;
		else {
			rmCheckList[i].func = new char[strlen(function.c_str())+1];
			for (unsigned j = 0; j < strlen(function.c_str()); j++)
				rmCheckList[i].func[j] = function[j];
			rmCheckList[i].func[strlen(function.c_str())] = '\0';
		}
		if (file.compare("0") == 0)
			rmCheckList[i].fname =  (char *) 0;
		else {
			rmCheckList[i].fname = new char[strlen(file.c_str()) +1];
			for (unsigned j = 0; j < strlen(file.c_str()); j++)
				rmCheckList[i].fname[j] = file[j];
			rmCheckList[i].fname[strlen(file.c_str())] = '\0';

		}
		rmCheckList[i].conversion = conv_bool;
		rmCheckList[i].overflow = overflow_bool;
		rmCheckList[i].shift = shift_bool;

	}
	whitelistFile.close();
}

void
InfoAppPass::setWrapper(CallInst *ci, Module &M, Function *func)
{
	FunctionType *ftype = func->getFunctionType();
	std::string fname = "__ioc_" + std::string(func->getName());

	Constant* ioc_wrapper = M.getOrInsertFunction(fname,
												  ftype,
												  func->getAttributes());
	ci->setCalledFunction(ioc_wrapper);

}

/*
 * Print Helpers
 */
void
InfoAppPass::format_ioc_report_func(const Value* val, raw_string_ostream& rs)
{
	const CallInst* ci = dyn_cast<CallInst>(val);
	assert(ci && "CallInst casting check");

	if (!xformMap[ci])
		return;

	const Function* func = ci->getCalledFunction();
	if (!func)
		return;
	assert(func && "Function casting check");

	//line & column
	dbg_err(func->getName());
	uint64_t line = getIntFromVal(ci->getOperand(0));
	uint64_t col = getIntFromVal(ci->getOperand(1));

	//XXX: restructure
	std::string fname = "";
	getStringFromVal(ci->getOperand(2), fname);

	rs << func->getName().str() << ":";
	rs << fname << ":" ;
	rs << " (line ";
	rs << line;
	rs << ", col ";
	rs << col << ")";

	//ioc_report_* specific items
	if (ioc_report_all_but_conv(func->getName()))
	{
		;
	} else if (func->getName() == "__ioc_report_conversion") {
		;
	} else {
		;
		//    assert(! "invalid function name");
	}
}

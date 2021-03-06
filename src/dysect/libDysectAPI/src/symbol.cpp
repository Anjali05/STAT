/*
Copyright (c) 2013-2014, Lawrence Livermore National Security, LLC.
Produced at the Lawrence Livermore National Laboratory.
Written by Niklas Nielsen, Gregory Lee [lee218@llnl.gov], Dong Ahn.
LLNL-CODE-645136.
All rights reserved.

This file is part of DysectAPI. For details, see https://github.com/lee218llnl/DysectAPI. Please also read dysect/LICENSE

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License (as published by the Free Software Foundation) version 2.1 dated February 1999.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the terms and conditions of the GNU General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along with this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include <LibDysectAPI.h>
#include <DysectAPI/Err.h>
#include <DysectAPI/Symbol.h>
#include <DysectAPI/Parser.h>

using namespace std;
using namespace DysectAPI;
using namespace Dyninst;
using namespace ProcControlAPI;
using namespace Stackwalker;
using namespace SymtabAPI;

bool DysectAPI::CodeLocation::addProcLib(Walker* proc) {
  if(procLibraries.find(proc) != procLibraries.end()) {
    return true;
  }

  LibraryState *libState = proc->getProcessState()->getLibraryTracker();
  if(!libState) {
    return DYSECTWARN(false, "Library state could not be retrieved");
  }

  vector<LibAddrPair> libs;
  if(!libState->getLibraries(libs)) {
    return DYSECTWARN(false, "Cannot get libraries from library state");
  }

  vector<LibAddrPair>::iterator libIter;

  bool found = false;
  for(libIter = libs.begin(); libIter != libs.end(); libIter++) {
    string curLibName = libIter->first;
    if(curLibName == libName) {
      Dyninst::Address addr = libIter->second;

      procLibraries.insert(pair<Walker*, Dyninst::Address>(proc, addr));
      found = true;
    }
  }

  return found;
}

bool DysectAPI::CodeLocation::getAddrs(Dyninst::Stackwalker::Walker* proc, std::vector<Dyninst::Address>& addrs) {
  map<Walker*, Dyninst::Address>::iterator procIter = procLibraries.find(proc);
  if(procLibraries.find(proc) == procLibraries.end()) {
    return DYSECTWARN(false, "Library offset for process cannot be found");
  }

  if(offsets.empty()) {
    return DYSECTWARN(false, "No relative symbol offsets present");
  }

  Dyninst::Address libOffset = procIter->second;
  map<Dyninst::Address, std::string*>::iterator offsetIter = offsets.begin();
  for(; offsetIter != offsets.end(); offsetIter++) {
    Dyninst::Address offset = offsetIter->first;
    Dyninst::Address addr = libOffset + offset;

    addrs.push_back(addr);
  }

  return true;
}

bool DysectAPI::CodeLocation::findSymbol(Dyninst::Stackwalker::Walker* proc, string name, vector<DysectAPI::CodeLocation*>& symbols) {
  vector<LibAddrPair> libs;

  if(SymbolTable::getLibraries(libs, proc) != OK) {
    return DYSECTWARN(false, "Could not get library list");
  }

  vector<LibAddrPair>::iterator libIter;

  bool isRegex = false;
  int pos = name.find_first_of("*?");
  if(pos != string::npos) {
    isRegex = true;
  }

  for(libIter = libs.begin(); libIter != libs.end(); libIter++) {
    string curLibName = libIter->first;
    Symtab* symtab = 0;

    if(SymbolTable::getSymbolTable(curLibName, symtab) != OK) {
      return DYSECTWARN(false, "Could not get symbol table");
    }

    findSymbol(symtab, name, curLibName, symbols, isRegex);
  }


  if(symbols.empty()) {
    return DYSECTLOG(false, "No symbols found for %s in current library set", name.c_str());
  }

  return true;
}

bool DysectAPI::CodeLocation::findSymbol(SymtabAPI::Symtab* symtab, string name, string libName, vector<DysectAPI::CodeLocation*>& symbols, bool isRegex) {
  assert(symtab != 0);

  bool exit = false;
  vector<SymtabAPI::Symbol *> symtabSymbols;
  vector<SymtabAPI::Symbol *> foundSymtabSymbols;
  set<pair<Dyninst::Address, string> > lOffsets;

  foundSymtabSymbols.clear();
  symtabSymbols.clear();

  // XXX: Include class specialization!
  if (name[0] == '~') {
    name.erase(0, 1);
    exit = true;
  }
  string symbolExpr = string(name);
  symbolExpr.append("<*>");
  //boost::regex expression(symbolExpr);

  if(!symtab->findSymbol(foundSymtabSymbols, name, SymtabAPI::Symbol::ST_FUNCTION, anyName, isRegex) && !isRegex) {

    // Try to search for template specialization
    symtab->findSymbol(foundSymtabSymbols, symbolExpr, SymtabAPI::Symbol::ST_FUNCTION, anyName, true);
  }

  if(foundSymtabSymbols.empty()) {
    //return Err::verbose(false, "No symbols found for '%s'", name.c_str());
    return false;
  }


  for(int i = 0; i < foundSymtabSymbols.size(); i++) {
    DysectAPI::CodeLocation* dsym = new DysectAPI::CodeLocation();
    dsym->libName = libName;

    SymtabAPI::Symbol* ssym = foundSymtabSymbols[i];

    string* str = new string(ssym->getPrettyName());
    Dyninst::Address offset = ssym->getOffset();
    if (exit == true) //TODO: This may not work if code optimized (i.e. MPI functions)
      offset = offset + ssym->getSize() - 1; // this is computing the exit!

    if(lOffsets.find(pair<Dyninst::Address, string> (offset, *str)) != lOffsets.end()) {
      continue;
    }
    lOffsets.insert(pair<Dyninst::Address, string> (offset, *str));

    dsym->offsets.insert(pair<Dyninst::Address, string*>(offset, str));

    symbols.push_back(dsym);
  }

  return true;
}

DysectAPI::CodeLocation::CodeLocation() {
}

DysectAPI::DysectErrorCode SymbolTable::getSymbolTable(std::string name, Symtab*& symtab) {

  map<string, Symtab*>::iterator symIter = symtabMap.find(name);

  if(symIter != symtabMap.end()) {
    symtab = symIter->second;
    return OK;
  }

  // XXX: Use broadcast instead of file-system read!
  int ret = Symtab::openFile(symtab, name);

  if(!ret || !symtab) {
    return DYSECTWARN(Error, "Cannot get symbol table for library '%s'", name.c_str());
  }

  symtabMap.insert(pair<string, Symtab*>(name, symtab));

  return OK;
}

DysectAPI::DysectErrorCode SymbolTable::getLibraries(vector<LibAddrPair>& libs, Walker* proc) {
  assert(proc);
  assert(proc->getProcessState());

  LibraryState *libState = proc->getProcessState()->getLibraryTracker();
  if(!libState) {
    DYSECTWARN(Error, "Couldn't get libstate. Trying to set default librrary tracket\n");
    proc->getProcessState()->setDefaultLibraryTracker();
    libState = proc->getProcessState()->getLibraryTracker();
    assert(libState);
  }
  if(!libState) {
    return DYSECTWARN(Error, "Library state could not be retrieved");
  }

  if(!libState->getLibraries(libs, true)) {
    return DYSECTWARN(Error, "Cannot get libraries from library state");
  }

  return OK;
}

DysectAPI::DysectErrorCode SymbolTable::findSymtabByName(string name, Walker* proc, Symtab*& symtab, string& libName) {
  vector<LibAddrPair> libs;

  if(getLibraries(libs, proc) != OK)
    return Error;

  vector<LibAddrPair>::iterator libIter;
  for(libIter = libs.begin(); libIter != libs.end(); libIter++) {
    string curLibName = libIter->first;

    vector<string> tokens = Parser::tokenize(curLibName, '/');

    if(tokens.empty())
      continue;

    string& lastToken = tokens[tokens.size() - 1];

    if(name.compare(lastToken) == 0) {
      if(getSymbolTable(curLibName, symtab) != OK)
        return Error;

      libName = curLibName;
      return OK;
    }
  }

  return Error;
}

bool CodeLocation::findFileLine(Dyninst::Stackwalker::Walker* proc, std::string name, int line, std::vector<DysectAPI::CodeLocation*>& locations) {
  vector<LibAddrPair> libs;

  if(SymbolTable::getLibraries(libs, proc) != OK) {
    return DYSECTWARN(false, "Could not get library list");
  }

  vector<LibAddrPair>::iterator libIter;

  for(libIter = libs.begin(); libIter != libs.end(); libIter++) {
    string curLibName = libIter->first;
    Symtab* symtab = 0;

    if(SymbolTable::getSymbolTable(curLibName, symtab) != OK) {
      return DYSECTWARN(false, "Could not get symbol table");
    }
//    symtab->setTruncateLinePaths(true);
    findFileLine(symtab, name, line, curLibName, locations);
  }


  if(locations.empty()) {
    return DYSECTWARN(false, "No symbols found for file %s & line %d", name.c_str(), line);
  }

  return true;
}


DysectAPI::DysectErrorCode SymbolTable::getLibraryByAddr(Dyninst::Stackwalker::LibAddrPair& lib, Dyninst::Address addr, Dyninst::Stackwalker::Walker* proc) {
  assert(proc);
  assert(proc->getProcessState());

  LibraryState *libState;
  bool ret;

  libState = proc->getProcessState()->getLibraryTracker();
  if(!libState) {
    return DYSECTWARN(Error, "Could not load library tracker");
  }

  if(!libState->getLibraryAtAddr(addr, lib)) {
    return DYSECTWARN(Error, "Failed to get library at address 0x%lx", addr);
  }

  return OK;
}

DysectAPI::DysectErrorCode SymbolTable::getFileLineByAddr(string& fileName, int& line, Dyninst::Address addr, Dyninst::Stackwalker::Walker* proc) {
  bool ret;
  LibAddrPair lib;
  vector<LineNoTuple *> lines;

  if(SymbolTable::getLibraryByAddr(lib, addr, proc) != OK) {
    fileName = "?";
    line = 0;
    return OK;
  }

  Symtab* symtab = 0;
  string libName = lib.first;
  if(SymbolTable::getSymbolTable(libName, symtab) != OK) {
    return DYSECTWARN(Error, "Could not load symbol table for library '%s'", libName.c_str());
  }

  symtab->setTruncateLinePaths(false);
  Address loadAddr = lib.second;
  ret = symtab->getSourceLines(lines, addr - loadAddr);
  if (!ret)
  {
    fileName = "?";
    line = 0;
    return OK;
  }

  fileName = string(lines[0]->getFile());
  line = lines[0]->getLine();

  return OK;
}

bool CodeLocation::findFileLine(Dyninst::SymtabAPI::Symtab* symtab, std::string name, int line, string libName, std::vector<DysectAPI::CodeLocation*>& locations) {
  assert(symtab != 0);

#if SW_MAJOR >= 9
  vector<AddressRange> ranges;
  vector<AddressRange>::iterator rangeIter;
#else
  vector<pair<Offset, Offset> > ranges;
  vector<pair<Offset, Offset> >::iterator rangeIter;
#endif

  if(!symtab->getAddressRanges(ranges, name, line)) {
//    return Err::warn(__LINE__, __FILE__, false, "getAddressRanges(.., %s, %s, %d) failed", libName.c_str(), name.c_str(), line);
    return false;
  }

  if(ranges.empty()) {
//    return Err::warn(__LINE__, __FILE__, false, "getAddressRanges(.., %s, %s, %d) empty!", libName.c_str(), name.c_str(), line);
    return false;
  }
  Err::warn(__LINE__, __FILE__, true, "YEAH! getAddressRanges(.., %s, %s, %d) suceeded", libName.c_str(), name.c_str(), line);

  DysectAPI::CodeLocation* location = new DysectAPI::CodeLocation();
  location->libName = libName;

  for(rangeIter = ranges.begin(); rangeIter != ranges.end(); ++rangeIter) {
    Offset start = (*rangeIter).first;

    location->offsets.insert(pair<Dyninst::Address, string*>(start, NULL));
  }

  locations.push_back(location);

  return true;
}


bool DataLocation::findVariable(Process::const_ptr process, Walker* proc, string name, DataLocation*& location) {
  assert(proc != 0);
  bool found = false;
  string function, variable;
  int frame = 0;

  // DataLocation expression parsed:
  //
  // loc_expr   ::= function:variable
  //             |  variable
  vector<string> tokens = Parser::tokenize(name, ':');
  int numTokens = tokens.size();
  if(numTokens == 1) {
    variable = tokens[0];
  }
  else if (numTokens == 2) {
    function = tokens[0];
    variable = tokens[1];
  }
  else {
    return DYSECTWARN(false, "Could not parse variable specification %s", name.c_str());
  }

//#if 0
  string baseStr, memberStr;
  int len = name.size();

  // XXX: Quick hack to get structure members
  // Should really be a part of the expression engine
  enum struct_parser {
    base,
    dash,
    gt,
    member
  } state = base;

  const char* str = name.c_str();

  for(int i = 0; i < len; i++) {
    char c = str[i];
    if((state == base) && (c == '-'))
      state = dash;
    else if((state == dash) && (c == '>'))
      state = member;
    else {
      if(state == base)
        baseStr += c;

      if(state == member)
        memberStr += c;
    }
  }

  if(!memberStr.empty()) {
    variable = baseStr;
  }
//#endif

  // 1st approach - look for local variable (resident in function for current frame)
  vector<Stackwalker::Frame> stackWalk;
  vector<localVar *> vars;
  vector<localVar *>::iterator varIter;

  if(!proc->walkStack(stackWalk)) {
    return DYSECTWARN(false, "Could not walk stack %s", Stackwalker::getLastErrorMsg());
  }

  Stackwalker::Frame curFrame;
  string frameName, simpleFrameName;
  if(numTokens == 1) {
    curFrame = stackWalk[0];
    curFrame.getName(frameName);
  }
  else {
    int i;
    for(i = 0; i < stackWalk.size(); i++) {
      curFrame = stackWalk[i];
      curFrame.getName(frameName);
      vector<string> tokens2 = Parser::tokenize(frameName, '(');
      frameName = tokens2[0];
      if(frameName == function)
        break;
    }
    if(i == stackWalk.size()) {
      return DYSECTWARN(false, "Failed to find frame '%s' for variable request %s", function.c_str(), name.c_str());
    }
    frame = i;
  }

  SymtabAPI::Function* func = getFunctionForFrame(curFrame);

  if (!func) {
    return DYSECTWARN(false, "Function for frame '%s' not found", frameName.c_str());
  }

  func->findLocalVariable(vars, variable);

  DYSECTVERBOSE(true, "Is variables for '%s' empty in frame[%d] %s? %s", variable.c_str(), frame, frameName.c_str(), vars.empty() ? "yes" : "no");

  if(!vars.empty()) {
    found = true;

    Module* mod = func->getModule();
    Symtab* symtab = 0;
    if(mod) {
      symtab = mod->exec();
    } else {
      return DYSECTWARN(false, "Could not find symbol table containing variable '%s'", variable.c_str());
    }

    location = new LocalVariableLocation(stackWalk, frame, vars[0], symtab);

//# if 0
    // XXX: Hack continued
    if(!memberStr.empty()) {
      Type* symType = location->getType();
      if(symType) {
        typeStruct *stType = symType->getStructType();
        if(stType) {
          vector<Field*>* fields = stType->getComponents();
          if(fields) {
            vector<Field*>::iterator fieldIter = fields->begin();

            for(;fieldIter != fields->end(); fieldIter++) {
              Field* field = *fieldIter;

              if(field && (field->getName().compare(memberStr) == 0)) {
                // Rebase
                Err::verbose(true, "Rebasing to member '%s' from %s", field->getName().c_str(), memberStr.c_str());
                int offset = field->getOffset() / 8;
                Type* fieldType = field->getType();

                DataLocation* fieldVariable = location->getInnerVariable(fieldType, offset);
                if(fieldVariable) {
                  location = fieldVariable;
                  return true;
                }
              }
            }
          }
        }
      }
    }
//# endif

    return true;
  }

  // Is parameter?
  if(func->getParams(vars)) {
    DYSECTVERBOSE(true, "Is parameters empty? %s", vars.empty() ? "yes" : "no");

    if(!vars.empty()) {
      varIter = vars.begin();
      for(;varIter != vars.end(); varIter++) {
        DYSECTVERBOSE(true, "Parameter found: '%s'", (*varIter)->getName().c_str());
      }
    }
  }

  if(func->getLocalVariables(vars)) {
    DYSECTVERBOSE(true, "Is locals empty? %s", vars.empty() ? "yes" : "no");

    if(!vars.empty()) {
      varIter = vars.begin();
      for(;varIter != vars.end(); varIter++) {
        DYSECTVERBOSE(true, "Local found: '%s'", (*varIter)->getName().c_str());
      }
    }
  } else {
    DYSECTVERBOSE(true, "Local variables not present");
  }

  if(!found) {
    // Try "global" variable / Available through symbol table
    vector<LibAddrPair> libs;
    vector<LibAddrPair>::iterator libIter;

    if(SymbolTable::getLibraries(libs, proc) != OK) {
      return DYSECTWARN(false, "Could not get libraries");
    }

    // Search through libraries
    for(libIter = libs.begin(); libIter != libs.end(); libIter++) {
      string curLibName = libIter->first;
      Dyninst::Address offset = libIter->second;

      Symtab* symtab = 0;

      if(SymbolTable::getSymbolTable(curLibName, symtab) != OK) {
        return DYSECTWARN(false, "Could not get symbol table");
      }

      if(findVariable(offset, symtab, process, variable, location)) {
        return true;
      }
    }
  }

  return DYSECTWARN(false, "Could not find variable %s in frame %s", variable.c_str(), frameName.c_str());
}

bool DataLocation::findVariable(Address libOffset,
                                Symtab* symtab,
                                Dyninst::ProcControlAPI::Process::const_ptr process,
                                string name,
                                DataLocation*& location) {

    vector<Variable *> variables;
    if (!symtab->findVariablesByName(variables, name)) {
      return false;
    }

    /* Multiple instances can not be distinguished */
    if(variables.size() != 1) {
       return DYSECTWARN(false, "Found unexpected number(%d) of instances for variable %s", variables.size(), name.c_str());
    }

    Variable *gvar = variables[0];
    Symbol *globalVar = gvar->getFirstSymbol();

    location = new GlobalVariableLocation(libOffset, process, globalVar, gvar, symtab);
    return true;

// GLL comment: Niklas' code replaced by Matt's above
//  vector<SymtabAPI::Symbol *> symbols;
//
//  if(!symtab->findSymbolByType(symbols, name, SymtabAPI::Symbol::ST_OBJECT)) {
//    return false;
//  }
//
//  /* Multiple instances can not be distinguished */
//  if(symbols.size() != 1) {
//    return Err::warn(false, "Found unexpected number(%d) of instances for variable %s", symbols.size(), name.c_str());
//  }
//
//  Symbol *globalVar = symbols[0];
//  Variable *gvar = NULL;
//
//  /* Find corresponding Variable structure*/
//  if(!symtab->findVariableByOffset(gvar, globalVar->getOffset())) {
//    return Err::warn(false, "Variable not found for symbol '%s'", name.c_str());
//  }
//
//  location = new GlobalVariableLocation(libOffset, process, globalVar, gvar, symtab);
//
//  return true;
}

bool DataLocation::isStructure() {
  Type* symType = getType();
  if(!symType)
    return false;

  typeStruct *stType = symType->getStructType();

  return (stType);
}

Value::content_t DataLocation::getDataType() {
  int i;
  const char *scalarTypeNames[] = {"int", "long", "float", "double", NULL};
  const char *scalarTypeName;
  bool pointer;
  Type *symType;
  Type *lookupType;
  typeScalar *stype, *type;

  if(dataType == Value::noType) {
    DYSECTINFO(true, "Determining type");

    pointer = false;
    symType = getType();
    stype = symType->getScalarType();
    type;
    if (!stype) {
      for (i = 0; scalarTypeNames[i] != NULL; i++) {
        scalarTypeName = scalarTypeNames[i];
        symtab->findType(lookupType, scalarTypeName);
        type = lookupType->getScalarType();

        if(symType->isCompatible(type)) {
          DYSECTVERBOSE(false, "Compatible with %s", scalarTypeName);
          stype = type;
          break;
        } else {
          DYSECTVERBOSE(false, "Not compatible with %s", scalarTypeName);
        }

        typeTypedef *typedefType = symType->getTypedefType();
        if(typedefType != NULL) {
          if(typedefType->isCompatible(type)) {
            DYSECTVERBOSE(false, "Typdef compatible with %s", scalarTypeName);
            stype = type;
            break;
          } else {
            DYSECTVERBOSE(false, "Typedef not compatible with %s", scalarTypeName);
          }
        }
      }
    }

    if(!stype) {
      typePointer* ptype = symType->getPointerType();
      if(ptype) {
        //XXX: Hack to print out pointer as long value
        DYSECTVERBOSE(false, "Pointer type");
        symtab->findType(lookupType, "long");
        type = lookupType->getScalarType();
        stype = type;
        pointer = true;
      }
    }

    if(stype) {
      string typeName = stype->getName();
      if (typeName == "int")
        dataType = Value::intType;
      else if (typeName == "long" || typeName == "long int") {
        if (pointer == true)
          dataType = Value::pointerType;
        else
          dataType = Value::longType;
      }
      else if (typeName == "float")
        dataType = Value::floatType;
      else if (typeName == "double")
        dataType = Value::doubleType;
      else if (typeName == "int *")
        dataType = Value::pointerType;
      else
        DYSECTWARN(false, "Unsupported data type: %s", typeName.c_str());
//      size_t typeSize = symType->getSize();
//      if(typeSize < sizeof(long)) {
//        dataType = Value::intType;
//      } else if(typeSize == sizeof(long)) {
//        dataType = Value::longType;
//      } else {
//        DYSECTWARN(false, "Scalar data size too big: %d", typeSize);
//      }
    } else {
      string& typeName = symType->getName();

      DYSECTWARN(false, "Currently unsupported data type: %s", typeName.c_str());
    }
  } else {
    DYSECTINFO(true, "Type already determined to %d", dataType);
  }

  return dataType;
}

DataLocation::DataLocation(LocationType locationType, Value::content_t dataType, Symtab* symtab) : locationType(locationType), dataType(dataType), symtab(symtab) {
}

LocalVariableLocation::LocalVariableLocation( std::vector<Stackwalker::Frame>& walkedStack,
                                              int frame,
                                              localVar* var,
                                              Symtab* symtab) : DataLocation(LocationLocal, Value::noType, symtab),
                                                                walkedStack(walkedStack),
                                                                frame(frame),
                                                                var(var) {
}

DysectAPI::DysectErrorCode LocalVariableLocation::getValue(Value& val) {
  DysectErrorCode code = OK;

  Type* symType = var->getType();
  size_t len = symType->getSize();
  val.buf = malloc(len);
  val.len = len;
  if(!val.buf) {
    DYSECTWARN(false, "Out of memory - buffer could not be allocated for fetching variable content");
  }

  int result = getLocalVariableValue(var, walkedStack, frame, val.buf, val.len);

  if(result == glvv_Success) {
    DYSECTVERBOSE(true, "Value read from local variable");
    code = OK;
  } else if(result == glvv_EUnknown) {
    code = OptimizedOut;
  } else {
    code = Error;
  }

  val.content = getDataType();

  return code;
}

Type* LocalVariableLocation::getType() {
  assert(var != 0);

  return var->getType();
}


DataLocation* LocalVariableLocation::getInnerVariable(Type *newType, int offset) {

  localVar *origVar = var;

  vector<VariableLocation>* newLocations = new vector<VariableLocation>();
  vector<VariableLocation> origLocs = origVar->getLocationLists();
  vector<VariableLocation>::iterator vit;

  for(vit = origLocs.begin(); vit != origLocs.end(); vit++) {
    VariableLocation& loc = *vit;
    if(loc.stClass == storageReg) {
      DYSECTVERBOSE(false, ">> Cannot adjust address of variable value");
      free(newLocations);
      return NULL;
    }

    /* Copy location information */
    VariableLocation newLoc(loc);

    /* Adjust offset */
    newLoc.frameOffset = loc.frameOffset + offset;

    newLocations->push_back(newLoc);
  }

  /* Create new localVar from container with new type and adjusted address */
  localVar *newVar = new localVar("", /* XXX: Get more meaning full name */
                                    newType,
                                    origVar->getFileName(),
                                    origVar->getLineNum(),
                                    NULL, /* Function unknown */
                                    newLocations
                                    );

  DataLocation* location = new LocalVariableLocation(walkedStack, 0, newVar, symtab);

  return location;
}

GlobalVariableLocation::GlobalVariableLocation( Address offset,
                                                Process::const_ptr process,
                                                SymtabAPI::Symbol *sym,
                                                Variable *var,
                                                Symtab* symtab) : DataLocation(LocationGlobal, Value::noType, symtab),
                                                                 offset(offset),
                                                                 process(process),
                                                                 sym(sym),
                                                                 var(var) {
}

Type* GlobalVariableLocation::getType() {
  assert(var != 0);

  return var->getType();
}

DataLocation* GlobalVariableLocation::getInnerVariable(Type *newType, int offset) {
  return 0;
}

DysectAPI::DysectErrorCode GlobalVariableLocation::getValue(Value& val) {
  assert(process != 0);

  DysectErrorCode code = OK;

  Type* symType = var->getType();
  size_t len = symType->getSize();
  val.buf = malloc(len);
  val.len = len;
  if(!val.buf) {
    DYSECTWARN(false, "Out of memory - buffer could not be allocated for fetching variable content");
  }

  if(!process->readMemory(val.buf, offset + sym->getOffset(), val.len)) {
    DYSECTWARN(false, "Failed to read memory length %d at offset %x + %x", val.len, offset, sym->getOffset());
    code = Error;
  }

  val.content = getDataType();

  return code;
}


bool DataLocation::getParams( vector<string>& params,
                              Walker* proc) {
  assert(proc != 0);
  bool found = false;

  vector<localVar *> vars;
  vector<localVar *>::iterator varIter;

  Stackwalker::Frame curFrame(proc);
  if(!proc->getInitialFrame(curFrame)) {
    return DYSECTWARN(false, "FuncParamNames could not get Initial Frame: %s", Stackwalker::getLastErrorMsg());
  }
  string frameName;
  if(!curFrame.getName(frameName))
    DYSECTWARN(false, "Failed to get frame name: %s", Stackwalker::getLastErrorMsg());

  SymtabAPI::Function* func = getFunctionForFrame(curFrame);

  if (!func) {
    return DYSECTWARN(false, "Function for frame '%s' not found", frameName.c_str());
  }

  func->getParams(vars);

  if(!vars.empty()) {
    varIter = vars.begin();
    for(; varIter != vars.end(); varIter++) {
      localVar* var = *varIter;

      if(var) {
        string name = var->getName();
        params.push_back(name);
      }
    }
  }

  return true;
}

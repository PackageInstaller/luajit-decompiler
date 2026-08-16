#include "../main.h"

Bytecode::Prototype::Prototype(const Bytecode& bytecode) : bytecode(bytecode) {}

void Bytecode::Prototype::operator()(std::vector<Prototype*>& unlinkedPrototypes) {
	read_header();
	read_instructions();
	read_upvalues();
	read_constants(unlinkedPrototypes);
	read_number_constants();
	read_debug_info();
	assert(prototypeSize == bytecode.fileBuffer.size(), "Prototype has unread bytes left", bytecode.filePath, DEBUG_INFO);
	unlinkedPrototypes.emplace_back(this);
}

void Bytecode::Prototype::read_header() {
	header.flags = get_next_byte();
	assert(!(header.flags & ~(BC_PROTO_CHILD | BC_PROTO_VARARG | BC_PROTO_FFI | BC_PROTO_BITOP)), "Prototype has invalid flags (" + byte_to_string(header.flags) + ")", bytecode.filePath, DEBUG_INFO);
	header.parameters = get_next_byte();
	header.framesize = get_next_byte();
	upvalues.resize(get_next_byte());
	constants.resize(get_uleb128());
	numberConstants.resize(get_uleb128());
	instructions.resize(get_uleb128());
	assert(instructions.size(), "Prototype has no instructions", bytecode.filePath, DEBUG_INFO);
	if (bytecode.header.flags & BC_F_STRIP || !get_uleb128()) return;
	header.hasDebugInfo = true;
	header.firstLine = get_uleb128();
	header.lineCount = get_uleb128();
}

void Bytecode::Prototype::read_instructions() {
	for (uint32_t i = 0; i < instructions.size(); i++) {
		if (bytecode.header.flags & BC_F_BE) {
			const uint8_t raw0 = get_next_byte();
			const uint8_t raw1 = get_next_byte();
			const uint8_t raw2 = get_next_byte();
			const uint8_t raw3 = get_next_byte();
			instructions[i].type = get_op_type(raw3, bytecode.header.version);
			instructions[i].a = raw2;

			if (is_op_abc_format(instructions[i].type)) {
				instructions[i].c = raw1;
				instructions[i].b = raw0;
			} else {
				instructions[i].d = (uint16_t)raw0 << 8 | raw1;
			}
		} else {
			instructions[i].type = get_op_type(get_next_byte(), bytecode.header.version);
			instructions[i].a = get_next_byte();

			if (is_op_abc_format(instructions[i].type)) {
				instructions[i].c = get_next_byte();
				instructions[i].b = get_next_byte();
			} else {
				instructions[i].d = get_next_byte();
				instructions[i].d |= (uint16_t)get_next_byte() << 8;
			}
		}

		assert(instructions[i].type < BC_OP_INVALID, "Prototype has invalid instruction (" + byte_to_string(instructions[i].type) + ")", bytecode.filePath, DEBUG_INFO);

		switch (instructions[i].type) {
		case BC_OP_ISTYPE:
		case BC_OP_ISNUM:
		case BC_OP_TGETR:
		case BC_OP_TSETR:
		case BC_OP_JFORI:
		case BC_OP_IFORL:
		case BC_OP_JFORL:
		case BC_OP_IITERL:
		case BC_OP_JITERL:
		case BC_OP_ILOOP:
		case BC_OP_JLOOP:
		case BC_OP_FUNCF:
		case BC_OP_IFUNCF:
		case BC_OP_JFUNCF:
		case BC_OP_FUNCV:
		case BC_OP_IFUNCV:
		case BC_OP_JFUNCV:
		case BC_OP_FUNCC:
		case BC_OP_FUNCCW:
			assert(false, "Prototype has unsupported instruction (" + byte_to_string(instructions[i].type) + ")", bytecode.filePath, DEBUG_INFO);
		}

	}
}

void Bytecode::Prototype::read_upvalues() {
	for (uint8_t i = 0; i < upvalues.size(); i++) {
		const uint8_t low = get_next_byte();
		const uint8_t high = get_next_byte();
		upvalues[i] = bytecode.header.flags & BC_F_BE
			? (uint16_t)low << 8 | high
			: (uint16_t)high << 8 | low;
	}
}

void Bytecode::Prototype::read_constants(std::vector<Prototype*>& unlinkedPrototypes) {
	uint32_t type;

	for (uint32_t i = 0; i < constants.size(); i++) {
		type = get_uleb128();

		switch (type) {
		case BC_KGC_CHILD:
			constants[i].type = BC_KGC_CHILD;
			assert(unlinkedPrototypes.size(), "Failed to link child prototype", bytecode.filePath, DEBUG_INFO);
			constants[i].prototype = unlinkedPrototypes.back();
			unlinkedPrototypes.pop_back();
			continue;
		case BC_KGC_TAB:
			constants[i].type = BC_KGC_TAB;
			constants[i].array.resize(get_uleb128());
			constants[i].table.resize(get_uleb128());

			for (uint32_t j = 0; j < constants[i].array.size(); j++) {
				constants[i].array[j] = get_table_constant();
			}

			for (uint32_t j = 0; j < constants[i].table.size(); j++) {
				constants[i].table[j].key = get_table_constant();
				constants[i].table[j].value = get_table_constant();
			}

			continue;
		case BC_KGC_COMPLEX:
			assert(!get_uleb128() && !get_uleb128(), "Prototype has invalid cdata constant", bytecode.filePath, DEBUG_INFO);
		case BC_KGC_I64:
		case BC_KGC_U64:
			constants[i].type = (BC_KGC)type;
			constants[i].cdata = get_uleb128();
			constants[i].cdata |= (uint64_t)get_uleb128() << 32;
			continue;
		default:
			constants[i].type = BC_KGC_STR;
			constants[i].string.resize(type - BC_KGC_STR);

			for (uint32_t j = 0; j < constants[i].string.size(); j++) {
				constants[i].string[j] = get_next_byte();
			}

			continue;
		}
	}
}

void Bytecode::Prototype::read_number_constants() {
	for (uint32_t i = 0; i < numberConstants.size(); i++) {
		if (bytecode.fileBuffer[prototypeSize] & 0x01) {
			numberConstants[i].type = BC_KNUM_NUM;
			numberConstants[i].number = get_uleb128_33();
			numberConstants[i].number |= (uint64_t)get_uleb128() << 32;
		} else {
			numberConstants[i].type = BC_KNUM_INT;
			numberConstants[i].integer = get_uleb128_33();
		}
	}
}

void Bytecode::Prototype::read_debug_info() {
	if (!header.hasDebugInfo) return;
	lineMap.resize(instructions.size());

	if (header.lineCount < 256) {
		for (uint32_t i = 0; i < lineMap.size(); i++) {
			lineMap[i] = get_next_byte();
		}
	} else if (header.lineCount < 65536) {
		for (uint32_t i = 0; i < lineMap.size(); i++) {
			const uint8_t first = get_next_byte();
			const uint8_t second = get_next_byte();
			lineMap[i] = bytecode.header.flags & BC_F_BE
				? (uint32_t)first << 8 | second
				: (uint32_t)second << 8 | first;
		}
	} else {
		for (uint32_t i = 0; i < lineMap.size(); i++) {
			const uint8_t byte0 = get_next_byte();
			const uint8_t byte1 = get_next_byte();
			const uint8_t byte2 = get_next_byte();
			const uint8_t byte3 = get_next_byte();
			lineMap[i] = bytecode.header.flags & BC_F_BE
				? (uint32_t)byte0 << 24 | (uint32_t)byte1 << 16 | (uint32_t)byte2 << 8 | byte3
				: (uint32_t)byte3 << 24 | (uint32_t)byte2 << 16 | (uint32_t)byte1 << 8 | byte0;
		}
	}

	upvalueNames.resize(upvalues.size());

	for (uint8_t i = 0; i < upvalueNames.size(); i++) {
		upvalueNames[i] = get_string();
	}

	uint32_t scopeOffset = 0, parameterCount = 0;

	for (uint8_t byte = get_next_byte(); byte; byte = get_next_byte()) {
		variableInfos.emplace_back();

		if (byte >= BC_VAR_STR) {
			variableInfos.back().type = BC_VAR_STR;
			variableInfos.back().name += byte;
			variableInfos.back().name += get_string();
		} else {
			variableInfos.back().type = (BC_VAR)byte;
		}

		scopeOffset += get_uleb128();
		assert(scopeOffset != 1, "Prototype variable has invalid scope", bytecode.filePath, DEBUG_INFO);

		if (!scopeOffset) {
			variableInfos.back().isParameter = true;
			parameterCount++;
			variableInfos.back().scopeEnd = get_uleb128() - 2;
		} else {
			variableInfos.back().scopeBegin = scopeOffset - 2;
			variableInfos.back().scopeEnd = variableInfos.back().scopeBegin + get_uleb128();
		}
	}

	assert(parameterCount == header.parameters, "Prototype parameter count does not\nmatch with debug info", bytecode.filePath, DEBUG_INFO);
	variableInfos.shrink_to_fit();
}

uint8_t Bytecode::Prototype::get_next_byte() {
	assert(prototypeSize < bytecode.fileBuffer.size(), "Prototype read would exceed end of buffer", bytecode.filePath, DEBUG_INFO);
	return bytecode.fileBuffer[prototypeSize++];
}

uint32_t Bytecode::Prototype::get_uleb128() {
	uint32_t uleb128 = get_next_byte();

	if (uleb128 >= 0x80) {
		uleb128 &= 0x7F;
		uint8_t byte, bitShift = 0;

		do {
			bitShift += 7;
			byte = get_next_byte();
			uleb128 |= (uint32_t)(byte & 0x7F) << bitShift;
		} while (byte >= 0x80);
	}

	return uleb128;
}

uint32_t Bytecode::Prototype::get_uleb128_33() {
	uint32_t uleb128_33 = get_next_byte() >> 1;

	if (uleb128_33 >= 0x40) {
		uleb128_33 &= 0x3F;
		uint8_t byte;
		int8_t bitShift = -1;

		do {
			bitShift += 7;
			byte = get_next_byte();
			uleb128_33 |= (uint32_t)(byte & 0x7F) << bitShift;
		} while (byte >= 0x80);
	}

	return uleb128_33;
}

std::string Bytecode::Prototype::get_string() {
	std::string string;

	for (uint8_t byte = get_next_byte(); byte; byte = get_next_byte()) {
		string += byte;
	}

	return string;
}

Bytecode::TableConstant Bytecode::Prototype::get_table_constant() {
	TableConstant tableConstant;
	const uint32_t type = get_uleb128();

	switch (type) {
	case BC_KTAB_NIL:
	case BC_KTAB_FALSE:
	case BC_KTAB_TRUE:
		tableConstant.type = (BC_KTAB)type;
		break;
	case BC_KTAB_INT:
		tableConstant.type = BC_KTAB_INT;
		tableConstant.integer = get_uleb128();
		break;
	case BC_KTAB_NUM:
		tableConstant.type = BC_KTAB_NUM;
		tableConstant.number = get_uleb128();
		tableConstant.number |= (uint64_t)get_uleb128() << 32;
		break;
	default:
		tableConstant.type = BC_KTAB_STR;
		tableConstant.string.resize(type - BC_KGC_STR);

		for (uint32_t i = 0; i < tableConstant.string.size(); i++) {
			tableConstant.string[i] = get_next_byte();
		}

		break;
	}

	return tableConstant;
}

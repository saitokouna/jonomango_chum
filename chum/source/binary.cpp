#include "binary.h"

#include <cassert>

#include <zycore/Format.h>

namespace chum {

static ZydisFormatterFunc orig_zydis_format_operand_mem = nullptr;
static ZydisFormatterFunc orig_zydis_format_operand_imm = nullptr;

static ZyanStatus hook_zydis_format_operand_mem(
    ZydisFormatter const* const formatter,
    ZydisFormatterBuffer* const buffer, ZydisFormatterContext* const context) {
  // Call the original function.
  if (context->operand->mem.base != ZYDIS_REGISTER_RIP)
    return orig_zydis_format_operand_mem(formatter, buffer, context);

  auto const& sym_table = *reinterpret_cast<std::vector<symbol*>*>(context->user_data);
  auto const sym = sym_table[context->operand->mem.disp.value];

  ZydisFormatterBufferAppend(buffer, ZYDIS_TOKEN_SYMBOL);
  ZyanString* string;
  ZydisFormatterBufferGetString(buffer, &string);
  ZyanStringAppendFormat(string, "%s", sym->name.c_str());

  return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus hook_zydis_format_operand_imm(
    ZydisFormatter const* const formatter,
    ZydisFormatterBuffer* const buffer, ZydisFormatterContext* const context) {
  // Call the original function.
  if (!context->operand->imm.is_relative)
    return orig_zydis_format_operand_imm(formatter, buffer, context);

  auto const& sym_table = *reinterpret_cast<std::vector<symbol*>*>(context->user_data);
  auto const sym = sym_table[context->operand->imm.value.u];

  ZydisFormatterBufferAppend(buffer, ZYDIS_TOKEN_SYMBOL);
  ZyanString* string;
  ZydisFormatterBufferGetString(buffer, &string);
  ZyanStringAppendFormat(string, "%s", sym->name.c_str());

  return ZYAN_STATUS_SUCCESS;
}

// Create an empty binary.
binary::binary() {
  // Initialize the Zydis decoder for x86-64.
  assert(ZYAN_SUCCESS(ZydisDecoderInit(&decoder_,
    ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)));

  // Initialize the Zydis formatter.
  assert(ZYAN_SUCCESS(ZydisFormatterInit(&formatter_,
    ZYDIS_FORMATTER_STYLE_INTEL)));

  // This makes it so that relative instructions are shown as RIP+X instead
  // of using an absolute address.
  ZydisFormatterSetProperty(&formatter_,
    ZYDIS_FORMATTER_PROP_FORCE_RELATIVE_BRANCHES, true);
  ZydisFormatterSetProperty(&formatter_,
    ZYDIS_FORMATTER_PROP_FORCE_RELATIVE_RIPREL, true);

  orig_zydis_format_operand_mem = hook_zydis_format_operand_mem;
  ZydisFormatterSetHook(&formatter_, ZYDIS_FORMATTER_FUNC_FORMAT_OPERAND_MEM,
    reinterpret_cast<void const**>(&orig_zydis_format_operand_mem));

  orig_zydis_format_operand_imm = hook_zydis_format_operand_imm;
  ZydisFormatterSetHook(&formatter_, ZYDIS_FORMATTER_FUNC_FORMAT_OPERAND_IMM,
    reinterpret_cast<void const**>(&orig_zydis_format_operand_imm));

  // Create the null symbol.
  auto const null_symbol = create_symbol(symbol_type::invalid, "<null>");
  assert(null_symbol->id == null_symbol_id);
}

// Free any resources.
binary::~binary() {
  for (auto const e : symbols_)
    delete e;
  for (auto const e : data_blocks_)
    delete e;
  for (auto const e : basic_blocks_)
    delete e;
}

// Move constructor.
binary::binary(binary&& other) {
  // I *think* this is correct...
  std::swap(decoder_,      other.decoder_);
  std::swap(formatter_,    other.formatter_);
  std::swap(symbols_,      other.symbols_);
  std::swap(data_blocks_,  other.data_blocks_);
  std::swap(basic_blocks_, other.basic_blocks_);
}

// Move assignment operator.
binary& binary::operator=(binary&& other) {
  // I *think* this is correct...
  std::swap(decoder_,      other.decoder_);
  std::swap(formatter_,    other.formatter_);
  std::swap(symbols_,      other.symbols_);
  std::swap(data_blocks_,  other.data_blocks_);
  std::swap(basic_blocks_, other.basic_blocks_);

  return *this;
}

// Print the contents of this binary, for debugging purposes.
void binary::print() {
  std::printf("[+] Symbols:\n");
  for (auto const sym : symbols_) {
    std::printf("[+]   ID: %-6u Type: %-8s",
      sym->id, serialize_symbol_type(sym->type));

    // Print the name, if it exists.
    if (!sym->name.empty())
      std::printf(" Name: %s\n", sym->name.c_str());
    else
      std::printf("\n");
  }

  std::printf("[+]\n[+] Data blocks:\n");
  for (std::size_t i = 0; i < data_blocks_.size(); ++i) {
    auto const db = data_blocks_[i];

    std::printf("[+]   #%-4zu Size: 0x%-8zX Alignment: 0x%-5X Read-only: %s\n",
      i, db->bytes.size(), db->alignment, db->read_only ? "true" : "false");
  }

  std::printf("[+]\n[+] Basic blocks:\n");
  for (std::size_t i = 0; i < basic_blocks_.size(); ++i) {
    auto const bb = basic_blocks_[i];

    std::printf("[+]   #%-4zd Symbol: %-6u Instruction count: %-4zu",
      i, bb->sym_id, bb->instructions.size());

    // Print the fallthrough target symbol ID, if it exists.
    if (bb->fallthrough_target)
      std::printf(" Fallthrough symbol: %-6u\n", bb->fallthrough_target->sym_id);
    else
      std::printf("\n");

    // Print every instruction.
    std::uint32_t instr_offset = 0;
    for (auto const& instr : bb->instructions) {
      ZydisDecodedInstruction decoded_instr;
      ZydisDecodedOperand decoded_operands[ZYDIS_MAX_OPERAND_COUNT_VISIBLE];

      ZydisDecoderDecodeFull(&decoder_, instr.bytes, instr.length,
        &decoded_instr, decoded_operands, ZYDIS_MAX_OPERAND_COUNT_VISIBLE,
        ZYDIS_DFLAG_VISIBLE_OPERANDS_ONLY);

      char buffer[128] = { 0 };
      ZydisFormatterFormatInstructionEx(&formatter_, &decoded_instr,
        decoded_operands, decoded_instr.operand_count_visible, buffer,
        128, 0, &symbols_);

      std::printf("[+]     +%.3X: %s\n", instr_offset, buffer);

      instr_offset += instr.length;
    }
  }
}

// Create a new symbol that is assigned a unique symbol ID.
symbol* binary::create_symbol(symbol_type const type, char const* const name) {
  auto const sym = symbols_.emplace_back(new symbol{});
  sym->id        = static_cast<symbol_id>(symbols_.size() - 1);
  sym->type      = type;
  sym->name      = name ? name : "";
  return sym;
}

// Create a zero-initialized data block of the specified size and alignment.
data_block* binary::create_data_block(
    std::uint32_t const size, std::uint32_t const alignment) {
  auto const db = data_blocks_.emplace_back(new data_block{});
  db->bytes     = std::vector<std::uint8_t>(size, 0);
  db->alignment = alignment;
  db->read_only = false;
  return db;
}

// Create and initialize a new data block from a raw blob of data.
data_block* binary::create_data_block(void const* const data,
    std::uint32_t const size, std::uint32_t const alignment) {
  // "Iterators" to pass to std::vector constructor.
  auto const data_begin = static_cast<std::uint8_t const*>(data);
  auto const data_end   = data_begin + size;

  auto const db = data_blocks_.emplace_back(new data_block{});
  db->bytes     = std::vector<std::uint8_t>(data_begin, data_end);
  db->alignment = alignment;
  db->read_only = false;
  return db;
}

// Create a new basic block for the specific code symbol. This block
// contains zero instructions upon creation.
basic_block* binary::create_basic_block(symbol_id const sym_id) {
  auto const bb = basic_blocks_.emplace_back(new basic_block{});
  bb->sym_id             = sym_id;
  bb->fallthrough_target = nullptr;
  bb->instructions       = {};
  return bb;
}

} // namespace chum

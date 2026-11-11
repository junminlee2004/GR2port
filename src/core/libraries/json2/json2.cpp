// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <map>
#include <memory>
#include <string>
#include <vector>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "common/types.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/json2/json2.h"
#include "core/libraries/libs.h"

// Gravity Rush 2 builds JSON values and serializes them; it never parses. Each guest object
// keeps a pointer to a host object in its first 8 bytes, and the game's allocator is not called.
namespace Libraries::Json2 {

enum class Type : u32 {
    Null,
    Boolean,
    Integer,
    UInteger,
    Real,
    String,
    Array,
    Object,
};

struct Node;
using Elements = std::vector<std::unique_ptr<Node>>;
using Members = std::map<std::string, std::unique_ptr<Node>>;

// sce::Json::Value, String, Array and Object with the sizes the game allocates.
struct Value {
    Node* node;
    u64 unused[3];
};
static_assert(sizeof(Value) == 0x20);

struct String {
    std::string* text;
};

struct Array {
    Elements* elements;
};

struct Object {
    Members* members;
};

// The content of a value. guest is the sce::Json::Value the game receives for an element of an
// array or a member of an object.
struct Node {
    Type type = Type::Null;
    bool boolean = false;
    u64 integer = 0;
    double real = 0.0;
    std::string string;
    Elements elements;
    Members members;
    Value guest{this, {}};

    Node() = default;
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
};

static void Assign(Node& to, const Node& from);

static std::unique_ptr<Node> Copy(const Node& from) {
    auto node = std::make_unique<Node>();
    Assign(*node, from);
    return node;
}

static Elements Copy(const Elements& from) {
    Elements elements;
    elements.reserve(from.size());
    for (const auto& element : from) {
        elements.push_back(Copy(*element));
    }
    return elements;
}

static Members Copy(const Members& from) {
    Members members;
    for (const auto& [key, member] : from) {
        members.emplace(key, Copy(*member));
    }
    return members;
}

// from may be to itself or a value inside to, so the copies are complete before to changes.
static void Assign(Node& to, const Node& from) {
    std::string string = from.string;
    Elements elements = Copy(from.elements);
    Members members = Copy(from.members);
    to.type = from.type;
    to.boolean = from.boolean;
    to.integer = from.integer;
    to.real = from.real;
    to.string = std::move(string);
    to.elements = std::move(elements);
    to.members = std::move(members);
}

// The escapes of the firmware's serializer.
static void WriteString(std::string& out, const std::string& string) {
    out += '"';
    for (const char c : string) {
        const u8 byte = static_cast<u8>(c);
        switch (byte) {
        case '\b':
            out += "\\b";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\r':
            out += "\\r";
            break;
        case '"':
            out += "\\\"";
            break;
        case '/':
            out += "\\/";
            break;
        case '\\':
            out += "\\\\";
            break;
        default:
            if (byte < 0x20 || byte == 0x7f) {
                out += fmt::format("\\u{:04x}", byte);
            } else {
                out += c;
            }
            break;
        }
    }
    out += '"';
}

// The firmware prints a real with six decimals and drops them when all six are zero.
static void WriteReal(std::string& out, double real) {
    std::string text = fmt::format("{:f}", real);
    const size_t point = text.find('.');
    if (point != std::string::npos && text.find_first_not_of('0', point + 1) == std::string::npos) {
        text.resize(point);
    }
    out += text;
}

static void Write(std::string& out, const Node& node) {
    switch (node.type) {
    case Type::Null:
        out += "null";
        break;
    case Type::Boolean:
        out += node.boolean ? "true" : "false";
        break;
    case Type::Integer:
        out += fmt::format("{}", static_cast<s64>(node.integer));
        break;
    case Type::UInteger:
        out += fmt::format("{}", node.integer);
        break;
    case Type::Real:
        WriteReal(out, node.real);
        break;
    case Type::String:
        WriteString(out, node.string);
        break;
    case Type::Array: {
        out += '[';
        bool first = true;
        for (const auto& element : node.elements) {
            if (!first) {
                out += ',';
            }
            first = false;
            Write(out, *element);
        }
        out += ']';
        break;
    }
    case Type::Object: {
        out += '{';
        bool first = true;
        for (const auto& [key, member] : node.members) {
            if (!first) {
                out += ',';
            }
            first = false;
            WriteString(out, key);
            out += ':';
            Write(out, *member);
        }
        out += '}';
        break;
    }
    }
}

static Node* NewNode(Type type) {
    auto* node = new Node;
    node->type = type;
    return node;
}

void PS4_SYSV_ABI ValueConstruct(Value* self) {
    self->node = NewNode(Type::Null);
}

void PS4_SYSV_ABI ValueConstructInteger(Value* self, s64 integer) {
    self->node = NewNode(Type::Integer);
    self->node->integer = static_cast<u64>(integer);
}

void PS4_SYSV_ABI ValueConstructUInteger(Value* self, u64 integer) {
    self->node = NewNode(Type::UInteger);
    self->node->integer = integer;
}

void PS4_SYSV_ABI ValueConstructReal(Value* self, double real) {
    self->node = NewNode(Type::Real);
    self->node->real = real;
}

void PS4_SYSV_ABI ValueConstructString(Value* self, const String* string) {
    self->node = NewNode(Type::String);
    self->node->string = *string->text;
}

void PS4_SYSV_ABI ValueConstructArray(Value* self, const Array* array) {
    self->node = NewNode(Type::Array);
    self->node->elements = Copy(*array->elements);
}

void PS4_SYSV_ABI ValueConstructObject(Value* self, const Object* object) {
    self->node = NewNode(Type::Object);
    self->node->members = Copy(*object->members);
}

void PS4_SYSV_ABI ValueDestruct(Value* self) {
    delete self->node;
}

Value* PS4_SYSV_ABI ValueAssign(Value* self, const Value* other) {
    Assign(*self->node, *other->node);
    return self;
}

void PS4_SYSV_ABI ValueSet(Value* self, const Value* other) {
    Assign(*self->node, *other->node);
}

void PS4_SYSV_ABI ValueClear(Value* self) {
    Assign(*self->node, Node{});
}

s32 PS4_SYSV_ABI ValueSerialize(Value* self, String* out) {
    Write(*out->text, *self->node);
    return ORBIS_OK;
}

void PS4_SYSV_ABI StringConstruct(String* self) {
    self->text = new std::string;
}

void PS4_SYSV_ABI StringConstructChars(String* self, const char* chars) {
    self->text = new std::string(chars);
}

void PS4_SYSV_ABI StringDestruct(String* self) {
    delete self->text;
}

const char* PS4_SYSV_ABI StringCStr(const String* self) {
    return self->text->c_str();
}

void PS4_SYSV_ABI ArrayConstruct(Array* self) {
    self->elements = new Elements;
}

void PS4_SYSV_ABI ArrayDestruct(Array* self) {
    delete self->elements;
}

void PS4_SYSV_ABI ArrayPushBack(Array* self, const Value* value) {
    self->elements->push_back(Copy(*value->node));
}

u64 PS4_SYSV_ABI ArraySize(const Array* self) {
    return self->elements->size();
}

void PS4_SYSV_ABI ObjectConstruct(Object* self) {
    self->members = new Members;
}

void PS4_SYSV_ABI ObjectConstructCopy(Object* self, const Object* other) {
    self->members = new Members(Copy(*other->members));
}

void PS4_SYSV_ABI ObjectDestruct(Object* self) {
    delete self->members;
}

Object* PS4_SYSV_ABI ObjectAssign(Object* self, const Object* other) {
    Members members = Copy(*other->members);
    *self->members = std::move(members);
    return self;
}

Value* PS4_SYSV_ABI ObjectSubscript(Object* self, const String* key) {
    auto& member = (*self->members)[*key->text];
    if (!member) {
        member = std::make_unique<Node>();
    }
    return &member->guest;
}

// Host memory holds every value, so the initializer and the game's allocator have no work.
void PS4_SYSV_ABI InitializerConstruct(void* self) {}

void PS4_SYSV_ABI InitializerDestruct(void* self) {}

s32 PS4_SYSV_ABI InitializerInitialize(void* self, const void* parameter) {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI InitializerTerminate(void* self) {
    return ORBIS_OK;
}

// The game's allocator class writes its own table of virtual functions after this returns.
void PS4_SYSV_ABI MemAllocatorConstruct(void* self) {}

void PS4_SYSV_ABI MemAllocatorDestruct(void* self) {}

void PS4_SYSV_ABI MemAllocatorNotifyError(void* self, s32 error, u64 size, void* user_data) {
    LOG_ERROR(Lib_Json2, "error = {:#x}, size = {}", error, size);
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    // sce::Json::Value
    LIB_FUNCTION("qBMjqyBn3OM", "libSceJson2", 1, "libSceJson", ValueConstruct);
    LIB_FUNCTION("0lLK8+kDqmE", "libSceJson2", 1, "libSceJson", ValueConstructInteger);
    LIB_FUNCTION("x4AUdbhpRB0", "libSceJson2", 1, "libSceJson", ValueConstructUInteger);
    LIB_FUNCTION("sOmU4vnx3s0", "libSceJson2", 1, "libSceJson", ValueConstructReal);
    LIB_FUNCTION("sZIoMRGO+jk", "libSceJson2", 1, "libSceJson", ValueConstructString);
    LIB_FUNCTION("iZeYfOxtMRg", "libSceJson2", 1, "libSceJson", ValueConstructArray);
    LIB_FUNCTION("3xUXnmUkXfo", "libSceJson2", 1, "libSceJson", ValueConstructObject);
    LIB_FUNCTION("WTtYf+cNnXI", "libSceJson2", 1, "libSceJson", ValueDestruct);
    LIB_FUNCTION("4zrm6VrgIAw", "libSceJson2", 1, "libSceJson", ValueAssign);
    LIB_FUNCTION("XL8+BUqjB1w", "libSceJson2", 1, "libSceJson", ValueSet);
    LIB_FUNCTION("FIjXN2TkuTs", "libSceJson2", 1, "libSceJson", ValueClear);
    LIB_FUNCTION("R7FDWtcN6f8", "libSceJson2", 1, "libSceJson", ValueSerialize);
    // sce::Json::String
    LIB_FUNCTION("qSmqLXXCPas", "libSceJson2", 1, "libSceJson", StringConstruct);
    LIB_FUNCTION("9KUZFjI1IxA", "libSceJson2", 1, "libSceJson", StringConstructChars);
    LIB_FUNCTION("cG1VE2HMl6c", "libSceJson2", 1, "libSceJson", StringDestruct);
    LIB_FUNCTION("L1KAkYWml-M", "libSceJson2", 1, "libSceJson", StringCStr);
    // sce::Json::Array
    LIB_FUNCTION("JP-PtKMiI1E", "libSceJson2", 1, "libSceJson", ArrayConstruct);
    LIB_FUNCTION("HJ8GpRT1aiw", "libSceJson2", 1, "libSceJson", ArrayDestruct);
    LIB_FUNCTION("zQtLRTqceMY", "libSceJson2", 1, "libSceJson", ArrayPushBack);
    LIB_FUNCTION("rQGJeNjOuUk", "libSceJson2", 1, "libSceJson", ArraySize);
    // sce::Json::Object
    LIB_FUNCTION("OJPTonqdg0I", "libSceJson2", 1, "libSceJson", ObjectConstruct);
    LIB_FUNCTION("a+W7HHlwpBs", "libSceJson2", 1, "libSceJson", ObjectConstructCopy);
    LIB_FUNCTION("5JmzZt8twAo", "libSceJson2", 1, "libSceJson", ObjectDestruct);
    LIB_FUNCTION("urOpESTBZmo", "libSceJson2", 1, "libSceJson", ObjectAssign);
    LIB_FUNCTION("ERuf9y0DY84", "libSceJson2", 1, "libSceJson", ObjectSubscript);
    // sce::Json::Initializer
    LIB_FUNCTION("cK6bYHf-Q5E", "libSceJson2", 1, "libSceJson", InitializerConstruct);
    LIB_FUNCTION("RujUxbr3haM", "libSceJson2", 1, "libSceJson", InitializerDestruct);
    LIB_FUNCTION("Cxwy7wHq4J0", "libSceJson2", 1, "libSceJson", InitializerInitialize);
    LIB_FUNCTION("PR5k1penBLM", "libSceJson2", 1, "libSceJson", InitializerTerminate);
    // sce::Json::MemAllocator
    LIB_FUNCTION("-hJRce8wn1U", "libSceJson2", 1, "libSceJson", MemAllocatorConstruct);
    LIB_FUNCTION("OcAgPxcq5Vk", "libSceJson2", 1, "libSceJson", MemAllocatorDestruct);
    LIB_FUNCTION("i-XwZjw0OOY", "libSceJson2", 1, "libSceJson", MemAllocatorNotifyError);
}

} // namespace Libraries::Json2

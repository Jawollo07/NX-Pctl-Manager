// Bounded, allocation-free JSON decoder for policy schema v1.
// This intentionally accepts a strict ASCII/integer-only JSON subset.
#pragma once

#include "policy.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace nxpctl::policy::json {

inline constexpr std::size_t MaxDocumentBytes = 16 * 1024;
inline constexpr std::size_t MaxTokens = 512;
inline constexpr std::size_t MaxNestingDepth = 8;
inline constexpr std::size_t MaxStringBytes = 128;

enum class Error : std::uint8_t {
    None = 0, NullInput, DocumentTooLarge, UnexpectedEnd, InvalidSyntax,
    TooManyTokens, NestingTooDeep, UnsupportedStringEscape, InvalidString,
    InvalidNumber, NumberOutOfRange, MissingField, UnknownField,
    DuplicateField, WrongType, ArraySize, PolicyInvalid
};

struct Token {
    enum class Type : std::uint8_t { Object, Array, String, Number, True, False, Null };
    Type type = Type::Null;
    std::size_t start = 0;
    std::size_t end = 0;
    std::uint16_t first_child = 0xFFFF;
    std::uint16_t next_sibling = 0xFFFF;
    std::uint16_t child_count = 0;
};

class Parser {
public:
    Parser(const char* input, std::size_t length) noexcept : input_(input), length_(length) {}

    [[nodiscard]] Error Parse(std::uint16_t* root) noexcept {
        if (input_ == nullptr || root == nullptr) return Error::NullInput;
        if (length_ == 0) return Error::UnexpectedEnd;
        if (length_ > MaxDocumentBytes) return Error::DocumentTooLarge;
        SkipWhitespace();
        if (!ParseValue(root, 0)) return error_;
        SkipWhitespace();
        return position_ == length_ ? Error::None : Error::InvalidSyntax;
    }

    [[nodiscard]] const Token& Get(std::uint16_t index) const noexcept { return tokens_[index]; }
    [[nodiscard]] const char* Input() const noexcept { return input_; }

private:
    static constexpr std::uint16_t InvalidToken = 0xFFFF;

    void SkipWhitespace() noexcept {
        while (position_ < length_) {
            const char c = input_[position_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++position_;
        }
    }

    std::uint16_t NewToken(Token::Type type, std::size_t start) noexcept {
        if (token_count_ >= MaxTokens) { error_ = Error::TooManyTokens; return InvalidToken; }
        const auto index = static_cast<std::uint16_t>(token_count_++);
        tokens_[index] = {};
        tokens_[index].type = type;
        tokens_[index].start = start;
        return index;
    }

    void AddChild(std::uint16_t parent, std::uint16_t child) noexcept {
        auto& p = tokens_[parent];
        if (p.first_child == InvalidToken) p.first_child = child;
        else {
            auto last = p.first_child;
            while (tokens_[last].next_sibling != InvalidToken) last = tokens_[last].next_sibling;
            tokens_[last].next_sibling = child;
        }
        ++p.child_count;
    }

    bool ParseString(std::uint16_t* result) noexcept {
        const auto start = position_;
        if (input_[position_++] != '"') return Fail(Error::InvalidSyntax);
        const auto token = NewToken(Token::Type::String, start + 1);
        if (token == InvalidToken) return false;
        while (position_ < length_) {
            const unsigned char c = static_cast<unsigned char>(input_[position_]);
            if (c == '"') {
                tokens_[token].end = position_++;
                if (tokens_[token].end - tokens_[token].start > MaxStringBytes)
                    return Fail(Error::InvalidString);
                *result = token;
                return true;
            }
            // Schema v1 identifiers and keys are printable ASCII without escapes.
            if (c < 0x20 || c > 0x7E || c == '\\') return Fail(Error::UnsupportedStringEscape);
            ++position_;
        }
        return Fail(Error::UnexpectedEnd);
    }

    bool ParseNumber(std::uint16_t* result) noexcept {
        const auto start = position_;
        if (input_[position_] == '-') ++position_;
        if (position_ >= length_ || input_[position_] < '0' || input_[position_] > '9')
            return Fail(Error::InvalidNumber);
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < length_ && input_[position_] >= '0' && input_[position_] <= '9')
                return Fail(Error::InvalidNumber);
        } else {
            while (position_ < length_ && input_[position_] >= '0' && input_[position_] <= '9')
                ++position_;
        }
        if (position_ < length_ && (input_[position_] == '.' || input_[position_] == 'e'
            || input_[position_] == 'E' || input_[position_] == '+'))
            return Fail(Error::InvalidNumber);
        const auto token = NewToken(Token::Type::Number, start);
        if (token == InvalidToken) return false;
        tokens_[token].end = position_;
        *result = token;
        return true;
    }

    bool ParseValue(std::uint16_t* result, std::size_t depth) noexcept {
        if (depth > MaxNestingDepth) return Fail(Error::NestingTooDeep);
        SkipWhitespace();
        if (position_ >= length_) return Fail(Error::UnexpectedEnd);
        const char c = input_[position_];
        if (c == '"') return ParseString(result);
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(result);
        if (c == '{' || c == '[') {
            const bool object = c == '{';
            const auto token = NewToken(object ? Token::Type::Object : Token::Type::Array, position_++);
            if (token == InvalidToken) return false;
            *result = token;
            SkipWhitespace();
            const char close = object ? '}' : ']';
            if (position_ < length_ && input_[position_] == close) {
                ++position_; tokens_[token].end = position_; return true;
            }
            while (position_ < length_) {
                std::uint16_t child = InvalidToken;
                if (object) {
                    if (input_[position_] != '"') return Fail(Error::InvalidSyntax);
                    if (!ParseString(&child)) return false;
                    AddChild(token, child);
                    SkipWhitespace();
                    if (position_ >= length_ || input_[position_++] != ':') return Fail(Error::InvalidSyntax);
                    SkipWhitespace();
                    if (!ParseValue(&child, depth + 1)) return false;
                    AddChild(token, child);
                } else {
                    if (!ParseValue(&child, depth + 1)) return false;
                    AddChild(token, child);
                }
                SkipWhitespace();
                if (position_ < length_ && input_[position_] == ',') {
                    ++position_; SkipWhitespace(); continue;
                }
                if (position_ < length_ && input_[position_] == close) {
                    ++position_; tokens_[token].end = position_; return true;
                }
                return Fail(Error::InvalidSyntax);
            }
            return Fail(Error::UnexpectedEnd);
        }
        const struct { const char* text; std::size_t size; Token::Type type; } literals[] = {
            {"true",4,Token::Type::True},{"false",5,Token::Type::False},{"null",4,Token::Type::Null}
        };
        for (const auto& literal : literals) {
            if (length_ - position_ >= literal.size) {
                bool match = true;
                for (std::size_t i=0;i<literal.size;++i) if(input_[position_+i]!=literal.text[i]) match=false;
                if (match) {
                    const auto token=NewToken(literal.type,position_);
                    if(token==InvalidToken)return false;
                    position_+=literal.size; tokens_[token].end=position_; *result=token; return true;
                }
            }
        }
        return Fail(Error::InvalidSyntax);
    }

    bool Fail(Error error) noexcept { if (error_ == Error::None) error_ = error; return false; }

    const char* input_;
    std::size_t length_;
    std::size_t position_ = 0;
    Token tokens_[MaxTokens]{};
    std::size_t token_count_ = 0;
    Error error_ = Error::None;
};

inline bool StringEquals(const Parser& parser, std::uint16_t token, const char* value) noexcept {
    const auto& t = parser.Get(token);
    if (t.type != Token::Type::String) return false;
    std::size_t n = 0;
    while (value[n] != '\0') ++n;
    if (t.end - t.start != n) return false;
    for (std::size_t i=0;i<n;++i) if (parser.Input()[t.start+i] != value[i]) return false;
    return true;
}

inline bool ReadUnsigned(const Parser& parser, std::uint16_t token, std::uint64_t* out) noexcept {
    const auto& t=parser.Get(token);
    if(t.type!=Token::Type::Number || t.start==t.end || parser.Input()[t.start]=='-') return false;
    std::uint64_t value=0;
    for(std::size_t i=t.start;i<t.end;++i) {
        const char c=parser.Input()[i];
        if(c<'0'||c>'9') return false;
        const auto digit=static_cast<std::uint64_t>(c-'0');
        if(value>(std::numeric_limits<std::uint64_t>::max()-digit)/10) return false;
        value=value*10+digit;
    }
    *out=value; return true;
}

inline bool ReadSigned(const Parser& parser, std::uint16_t token, std::int64_t* out) noexcept {
    const auto& t=parser.Get(token);
    if(t.type!=Token::Type::Number || t.start==t.end) return false;
    const bool negative=parser.Input()[t.start]=='-';
    std::size_t i=t.start+(negative?1:0);
    std::uint64_t value=0;
    for(;i<t.end;++i) {
        const char c=parser.Input()[i];
        if(c<'0'||c>'9') return false;
        const auto digit=static_cast<std::uint64_t>(c-'0');
        if(value>(std::numeric_limits<std::uint64_t>::max()-digit)/10) return false;
        value=value*10+digit;
    }
    const auto max=static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if ((!negative && value>max) || (negative && value>max+1)) return false;
    if(negative && value==max+1) *out=std::numeric_limits<std::int64_t>::min();
    else *out=negative?-static_cast<std::int64_t>(value):static_cast<std::int64_t>(value);
    return true;
}

inline bool ReadBool(const Parser& parser, std::uint16_t token, bool* out) noexcept {
    const auto type=parser.Get(token).type;
    if(type==Token::Type::True){*out=true;return true;}
    if(type==Token::Type::False){*out=false;return true;}
    return false;
}

inline Error Decode(const char* input, std::size_t length, Policy* out) noexcept {
    if(out==nullptr) return Error::NullInput;
    Parser parser(input,length);
    std::uint16_t root=0xFFFF;
    auto error=parser.Parse(&root);
    if(error!=Error::None)return error;
    if(parser.Get(root).type!=Token::Type::Object)return Error::WrongType;

    Policy candidate{};
    bool seen[16]{};
    auto child=parser.Get(root).first_child;
    const auto child_count=parser.Get(root).child_count;
    if((child_count%2)!=0)return Error::InvalidSyntax;
    for(std::size_t pair=0;pair<child_count/2;++pair) {
        const auto key=child;
        if(key==0xFFFF)return Error::InvalidSyntax;
        const auto value=parser.Get(key).next_sibling;
        if(value==0xFFFF)return Error::InvalidSyntax;
        child=parser.Get(value).next_sibling;
        int field=-1;
        const char* names[]={"protocol_version","schema_version","device_id","revision","issued_at","not_after","server_time","enabled","require_parent_authentication","offline_behavior","timezone_offset_minutes","global_daily_limit_minutes","weekday_limit_minutes","quiet_hours_enabled","quiet_hours_start_minute","quiet_hours_end_minute"};
        for(int i=0;i<16;++i) if(StringEquals(parser,key,names[i])) {field=i;break;}
        if(field<0)return Error::UnknownField;
        if(seen[field])return Error::DuplicateField;
        seen[field]=true;
        std::uint64_t u=0;
        std::int64_t s=0;
        switch(field) {
            case 0: if(!ReadUnsigned(parser,value,&u)||u>UINT32_MAX)return Error::WrongType; candidate.protocol_version=static_cast<std::uint32_t>(u); break;
            case 1: if(!ReadUnsigned(parser,value,&u)||u>UINT32_MAX)return Error::WrongType; candidate.schema_version=static_cast<std::uint32_t>(u); break;
            case 2: {
                const auto& t=parser.Get(value);
                if(t.type!=Token::Type::String || t.end-t.start==0 || t.end-t.start>=DeviceIdCapacity)return Error::WrongType;
                for(std::size_t i=0;i<t.end-t.start;++i) candidate.device_id[i]=parser.Input()[t.start+i];
                candidate.device_id[t.end-t.start]='\0'; break;
            }
            case 3: if(!ReadUnsigned(parser,value,&candidate.revision))return Error::WrongType; break;
            case 4: if(!ReadSigned(parser,value,&candidate.issued_at))return Error::WrongType; break;
            case 5: if(!ReadSigned(parser,value,&candidate.not_after))return Error::WrongType; break;
            case 6: if(!ReadSigned(parser,value,&candidate.server_time))return Error::WrongType; break;
            case 7: if(!ReadBool(parser,value,&candidate.enabled))return Error::WrongType; break;
            case 8: if(!ReadBool(parser,value,&candidate.require_parent_authentication))return Error::WrongType; break;
            case 9: if(!ReadUnsigned(parser,value,&u)||u>1)return Error::WrongType; candidate.offline_behavior=static_cast<OfflineBehavior>(u); break;
            case 10: if(!ReadSigned(parser,value,&s)||s<INT16_MIN||s>INT16_MAX)return Error::NumberOutOfRange; candidate.timezone_offset_minutes=static_cast<std::int16_t>(s); break;
            case 11: if(!ReadUnsigned(parser,value,&u)||u>UINT16_MAX)return Error::NumberOutOfRange; candidate.global_daily_limit_minutes=static_cast<std::uint16_t>(u); break;
            case 12: {
                if(parser.Get(value).type!=Token::Type::Array||parser.Get(value).child_count!=7)return Error::ArraySize;
                auto item=parser.Get(value).first_child;
                for(std::size_t i=0;i<7;++i){if(!ReadUnsigned(parser,item,&u)||u>UINT16_MAX)return Error::WrongType;candidate.weekday_limit_minutes[i]=static_cast<std::uint16_t>(u);item=parser.Get(item).next_sibling;}
                break;
            }
            case 13: if(!ReadBool(parser,value,&candidate.quiet_hours_enabled))return Error::WrongType; break;
            case 14: if(!ReadUnsigned(parser,value,&u)||u>UINT16_MAX)return Error::NumberOutOfRange; candidate.quiet_hours_start_minute=static_cast<std::uint16_t>(u); break;
            case 15: if(!ReadUnsigned(parser,value,&u)||u>UINT16_MAX)return Error::NumberOutOfRange; candidate.quiet_hours_end_minute=static_cast<std::uint16_t>(u); break;
        }
    }
    for(bool present:seen) if(!present)return Error::MissingField;
    *out=candidate;
    return Error::None;
}

inline Error DecodeAndValidate(
    const char* input, std::size_t length, const ValidationContext& context,
    Policy* out) noexcept {
    if(out==nullptr)return Error::NullInput;
    Policy candidate{};
    const auto error=Decode(input,length,&candidate);
    if(error!=Error::None)return error;
    if(Validate(candidate,context)!=ValidationError::None)return Error::PolicyInvalid;
    *out=candidate;
    return Error::None;
}

} // namespace nxpctl::policy::json

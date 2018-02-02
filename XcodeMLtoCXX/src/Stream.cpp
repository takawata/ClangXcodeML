#include <cassert>
#include <memory>
#include <sstream>
#include <string>

#include "Stream.h"

namespace {

template <typename T, typename... Ts>
std::unique_ptr<T>
make_unique(Ts &&... params) {
  return std::unique_ptr<T>(new T(std::forward<Ts>(params)...));
}

} // namespace

namespace CXXCodeGen {

const space_t space = {};

const newline_t newline = {};

struct StreamImpl {
  StreamImpl() : ss(), curIndent(0), alreadyIndented(false), lastChar('\n') {
  }

  std::stringstream ss;
  size_t curIndent;
  bool alreadyIndented;
  char lastChar;
};

namespace {

void
outputIndentation(StreamImpl &impl) {
  if (impl.alreadyIndented) {
    return;
  }
  impl.ss << std::string(impl.curIndent, '\t');
  impl.lastChar = '\t';
  impl.alreadyIndented = true;
}

void
emit(StreamImpl &impl, const std::string &str) {
  if (str.empty()) {
    return;
  }
  impl.ss << str;
  impl.lastChar = str.back();
}

} // namespace

Stream::Stream() : pimpl(make_unique<StreamImpl>()) {
}

Stream::~Stream() = default;

Stream::Stream(Stream &&) = default;

Stream &Stream::operator=(Stream &&) = default;

std::string
Stream::str() {
  return pimpl->ss.str();
}

void
Stream::indent(size_t amount) {
  pimpl->curIndent += amount;
}

void
Stream::unindent(size_t amount) {
  assert(pimpl->curIndent >= amount);
  pimpl->curIndent -= amount;
}

void
Stream::insertSpace() {
  const std::string separators = "\n\t ";
  if (separators.find(pimpl->lastChar) == std::string::npos) {
    emit(*pimpl, " ");
  }
}

void
Stream::insertNewLine() {
  emit(*pimpl, "\n");
  pimpl->alreadyIndented = false;
}

namespace {

bool
isAllowedInIdent(char c) {
  /* FIXME: C++ allows universal character */
  return isalnum(c) || c == '_';
}

bool
shouldInterleaveSpace(char last, char next) {
  const std::string operators = "+-*/%^&|!><";
  const std::string repeatables = "+-><&|=";
  return (isAllowedInIdent(last) && isAllowedInIdent(next))
      || (operators.find(last) != std::string::npos && next == '=')
      || (last == next && repeatables.find(last) != std::string::npos)
      || (next == ':')
      /* `label1:::i = 1;`
       * `void::Class1::func() { }`
       */
      || (last == '-' && next == '>') || // `->`
      (last == '>' && next == '*'); // `->*`
}

} // namespace

void
Stream::insert(const std::string &token) {
  if (token.empty()) {
    return;
  }

  outputIndentation(*pimpl);

  if (shouldInterleaveSpace(pimpl->lastChar, token[0])) {
    emit(*pimpl, " ");
  }
  emit(*pimpl, token);
}

} // namespace CXXCodeGen

CXXCodeGen::Stream &operator<<(
    CXXCodeGen::Stream &s, const CXXCodeGen::space_t &) {
  s.insertSpace();
  return s;
}

CXXCodeGen::Stream &operator<<(
    CXXCodeGen::Stream &s, const CXXCodeGen::newline_t &) {
  s.insertNewLine();
  return s;
}

CXXCodeGen::Stream &operator<<(CXXCodeGen::Stream &s, const std::string &str) {
  s.insert(str);
  return s;
}

CXXCodeGen::Stream &operator<<(CXXCodeGen::Stream &s, char c) {
  s.insert(std::string(1, c));
  return s;
}

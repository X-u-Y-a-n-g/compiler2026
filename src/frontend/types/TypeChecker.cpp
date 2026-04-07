#include "compiler/frontend/types/TypeChecker.h"

namespace compiler::frontend::types {

std::string Type::toString() const {
  switch (kind) {
    case TypeKind::Void:
      return "void";
    case TypeKind::Int:
      return "int";
    case TypeKind::Float:
      return "float";
    case TypeKind::Bool:
      return "bool";
    case TypeKind::Long:
      return "long";
    case TypeKind::Unknown:
      return "unknown";
  }
  return "unknown";
}

bool TypeChecker::check(const compiler::frontend::ast::TranslationUnit&,
                        compiler::support::DiagnosticEngine&) const {
  return true;
}

}  // namespace compiler::frontend::types

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace compiler::ir::core {

enum class ConstantKind {
  Scalar,
  Aggregate,
};

class Constant {
 public:
  static Constant scalar(std::string type, std::string literal) {
    return Constant(ConstantKind::Scalar, std::move(type), std::move(literal), {});
  }

  static Constant aggregate(std::string type, std::vector<Constant> elements) {
    return Constant(ConstantKind::Aggregate, std::move(type), "", std::move(elements));
  }

  [[nodiscard]] ConstantKind kind() const { return kind_; }
  [[nodiscard]] bool isScalar() const { return kind_ == ConstantKind::Scalar; }
  [[nodiscard]] bool isAggregate() const { return kind_ == ConstantKind::Aggregate; }
  [[nodiscard]] const std::string& type() const { return type_; }
  [[nodiscard]] const std::string& literal() const { return literal_; }
  [[nodiscard]] const std::vector<Constant>& elements() const { return elements_; }

 private:
  Constant(ConstantKind kind, std::string type, std::string literal, std::vector<Constant> elements)
      : kind_(kind),
        type_(std::move(type)),
        literal_(std::move(literal)),
        elements_(std::move(elements)) {}

  ConstantKind kind_ = ConstantKind::Scalar;
  std::string type_;
  std::string literal_;
  std::vector<Constant> elements_;
};

}  // namespace compiler::ir::core

/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "packet_def.h"

#include <list>
#include <set>

#include "fields/all_fields.h"
#include "util.h"

PacketDef::PacketDef(std::string name, FieldList fields) : ParentDef(name, fields, nullptr){};
PacketDef::PacketDef(std::string name, FieldList fields, PacketDef* parent) : ParentDef(name, fields, parent){};

PacketField* PacketDef::GetNewField(const std::string&, ParseLocation) const {
  return nullptr;  // Packets can't be fields
}

void PacketDef::GenParserDefinition(std::ostream& s) const {
  s << "class " << name_ << "View";
  if (parent_ != nullptr) {
    s << " : public " << parent_->name_ << "View {";
  } else {
    s << " : public PacketView<" << (is_little_endian_ ? "" : "!") << "kLittleEndian> {";
  }
  s << " public:";

  // Specialize function
  if (parent_ != nullptr) {
    s << "static " << name_ << "View Create(" << parent_->name_ << "View parent)";
    s << "{ return " << name_ << "View(parent); }";
  } else {
    s << "static " << name_ << "View Create(PacketView<" << (is_little_endian_ ? "" : "!") << "kLittleEndian> packet) ";
    s << "{ return " << name_ << "View(packet); }";
  }

  std::set<std::string> fixed_types = {
      FixedScalarField::kFieldType,
      FixedEnumField::kFieldType,
  };

  // Print all of the public fields which are all the fields minus the fixed fields.
  const auto& public_fields = fields_.GetFieldsWithoutTypes(fixed_types);
  bool has_fixed_fields = public_fields.size() != fields_.size();
  for (const auto& field : public_fields) {
    GenParserFieldGetter(s, field);
    s << "\n";
  }
  GenValidator(s);
  s << "\n";

  s << " protected:\n";
  // Constructor from a View
  if (parent_ != nullptr) {
    s << name_ << "View(" << parent_->name_ << "View parent)";
    s << " : " << parent_->name_ << "View(parent) { was_validated_ = false; }";
  } else {
    s << name_ << "View(PacketView<" << (is_little_endian_ ? "" : "!") << "kLittleEndian> packet) ";
    s << " : PacketView<" << (is_little_endian_ ? "" : "!") << "kLittleEndian>(packet) { was_validated_ = false;}";
  }

  // Print the private fields which are the fixed fields.
  if (has_fixed_fields) {
    const auto& private_fields = fields_.GetFieldsWithTypes(fixed_types);
    s << " private:\n";
    for (const auto& field : private_fields) {
      GenParserFieldGetter(s, field);
      s << "\n";
    }
  }
  s << "};\n";
}

void PacketDef::GenParserFieldGetter(std::ostream& s, const PacketField* field) const {
  // Start field offset
  auto start_field_offset = GetOffsetForField(field->GetName(), false);
  auto end_field_offset = GetOffsetForField(field->GetName(), true);

  if (start_field_offset.empty() && end_field_offset.empty()) {
    std::cerr << "Field location for " << field->GetName() << " is ambiguous, "
              << "no method exists to determine field location from begin() or end().\n";
    abort();
  }

  field->GenGetter(s, start_field_offset, end_field_offset);
}

TypeDef::Type PacketDef::GetDefinitionType() const {
  return TypeDef::Type::PACKET;
}

void PacketDef::GenValidator(std::ostream& s) const {
  // Get the static offset for all of our fields.
  int bits_size = 0;
  for (const auto& field : fields_) {
    bits_size += field->GetSize().bits();
  }

  // Write the function declaration.
  s << "virtual bool IsValid() " << (parent_ != nullptr ? " override" : "") << " {";
  s << "if (was_validated_) { return true; } ";
  s << "else { was_validated_ = true; was_validated_ = IsValid_(); return was_validated_; }";
  s << "}";

  s << "protected:";
  s << "virtual bool IsValid_() const {";

  // Offset by the parents known size. We know that any dynamic fields can
  // already be called since the parent must have already been validated by
  // this point.
  auto parent_size = Size(0);
  if (parent_ != nullptr) {
    parent_size = parent_->GetSize(true);
  }

  s << "auto it = begin() + (" << parent_size << ") / 8;";

  // Check if you can extract the static fields.
  // At this point you know you can use the size getters without crashing
  // as long as they follow the instruction that size fields cant come before
  // their corrisponding variable length field.
  s << "it += " << ((bits_size + 7) / 8) << " /* Total size of the fixed fields */;";
  s << "if (it > end()) return false;";

  // For any variable length fields, use their size check.
  for (const auto& field : fields_) {
    if (field->GetFieldType() == ChecksumStartField::kFieldType) {
      auto offset = GetOffsetForField(field->GetName(), false);
      if (!offset.empty()) {
        s << "size_t sum_index = (" << offset << ") / 8;";
      } else {
        offset = GetOffsetForField(field->GetName(), true);
        if (offset.empty()) {
          ERROR(field) << "Checksum Start Field offset can not be determined.";
        }
        s << "size_t sum_index = size() - (" << offset << ") / 8;";
      }

      const auto& field_name = ((ChecksumStartField*)field)->GetStartedFieldName();
      const auto& started_field = fields_.GetField(field_name);
      if (started_field == nullptr) {
        ERROR(field) << __func__ << ": Can't find checksum field named " << field_name << "(" << field->GetName()
                     << ")";
      }
      auto end_offset = GetOffsetForField(started_field->GetName(), false);
      if (!end_offset.empty()) {
        s << "size_t end_sum_index = (" << end_offset << ") / 8;";
      } else {
        end_offset = GetOffsetForField(started_field->GetName(), true);
        if (end_offset.empty()) {
          ERROR(started_field) << "Checksum Field end_offset can not be determined.";
        }
        s << "size_t end_sum_index = size() - (" << started_field->GetSize() << " - " << end_offset << ") / 8;";
      }
      if (is_little_endian_) {
        s << "auto checksum_view = GetLittleEndianSubview(sum_index, end_sum_index);";
      } else {
        s << "auto checksum_view = GetBigEndianSubview(sum_index, end_sum_index);";
      }
      s << started_field->GetDataType() << " checksum;";
      s << "checksum.Initialize();";
      s << "for (uint8_t byte : checksum_view) { ";
      s << "checksum.AddByte(byte);}";
      s << "if (checksum.GetChecksum() != (begin() + end_sum_index).extract<"
        << util::GetTypeForSize(started_field->GetSize().bits()) << ">()) { return false; }";

      continue;
    }

    auto field_size = field->GetSize();
    // Fixed size fields have already been handled.
    if (!field_size.has_dynamic()) {
      continue;
    }

    // Custom fields with dynamic size must have the offset for the field passed in as well
    // as the end iterator so that they may ensure that they don't try to read past the end.
    // Custom fields with fixed sizes will be handled in the static offset checking.
    if (field->GetFieldType() == CustomField::kFieldType) {
      // Check if we can determine offset from begin(), otherwise error because by this point,
      // the size of the custom field is unknown and can't be subtracted from end() to get the
      // offset.
      auto offset = GetOffsetForField(field->GetName(), false);
      if (offset.empty()) {
        ERROR(field) << "Custom Field offset can not be determined from begin().";
      }

      if (offset.bits() % 8 != 0) {
        ERROR(field) << "Custom fields must be byte aligned.";
      }

      // Custom fields are special as their size field takes an argument.
      const auto& custom_size_var = field->GetName() + "_size";
      s << "const auto& " << custom_size_var << " = " << field_size.dynamic_string();
      s << "(begin() + (" << offset << ") / 8);";

      s << "if (!" << custom_size_var << ".has_value()) { return false; }";
      s << "it += *" << custom_size_var << ";";
      s << "if (it > end()) return false;";
      continue;
    } else {
      s << "it += (" << field_size.dynamic_string() << ") / 8;";
      s << "if (it > end()) return false;";
    }
  }

  // Validate constraints after validating the size
  if (parent_constraints_.size() > 0 && parent_ == nullptr) {
    ERROR() << "Can't have a constraint on a NULL parent";
  }

  for (const auto& constraint : parent_constraints_) {
    s << "if (Get" << util::UnderscoreToCamelCase(constraint.first) << "() != ";
    const auto& field = parent_->GetParamList().GetField(constraint.first);
    if (field->GetFieldType() == ScalarField::kFieldType) {
      s << std::get<int64_t>(constraint.second);
    } else {
      s << std::get<std::string>(constraint.second);
    }
    s << ") return false;";
  }

  // Validate the packets fields last
  for (const auto& field : fields_) {
    field->GenValidator(s);
    s << "\n";
  }

  s << "return true;";
  s << "}\n";
  if (parent_ == nullptr) {
    s << "bool was_validated_{false};\n";
  }
}

void PacketDef::GenBuilderDefinition(std::ostream& s) const {
  s << "class " << name_ << "Builder";
  if (parent_ != nullptr) {
    s << " : public " << parent_->name_ << "Builder";
  } else {
    if (is_little_endian_) {
      s << " : public PacketBuilder<kLittleEndian>";
    } else {
      s << " : public PacketBuilder<!kLittleEndian>";
    }
  }
  s << " {";
  s << " public:";
  s << "  virtual ~" << name_ << "Builder()" << (parent_ != nullptr ? " override" : "") << " = default;";

  if (!fields_.HasBody()) {
    GenBuilderCreate(s);
    s << "\n";
  }

  GenSerialize(s);
  s << "\n";

  GenSize(s);
  s << "\n";

  s << " protected:\n";
  GenBuilderConstructor(s);
  s << "\n";

  GenBuilderParameterChecker(s);
  s << "\n";

  GenMembers(s);
  s << "};\n";
}

FieldList PacketDef::GetParametersToValidate() const {
  FieldList params_to_validate;
  for (const auto& field : GetParamList()) {
    if (field->HasParameterValidator()) {
      params_to_validate.AppendField(field);
    }
  }
  return params_to_validate;
}

void PacketDef::GenBuilderCreate(std::ostream& s) const {
  s << "static std::unique_ptr<" << name_ << "Builder> Create(";

  auto params = GetParamList();
  for (int i = 0; i < params.size(); i++) {
    params[i]->GenBuilderParameter(s);
    if (i != params.size() - 1) {
      s << ", ";
    }
  }
  s << ") {";

  // Call the constructor
  s << "auto builder = std::unique_ptr<" << name_ << "Builder>(new " << name_ << "Builder(";

  params = params.GetFieldsWithoutTypes({
      PayloadField::kFieldType,
      BodyField::kFieldType,
  });
  // Add the parameters.
  for (int i = 0; i < params.size(); i++) {
    s << params[i]->GetName();
    if (i != params.size() - 1) {
      s << ", ";
    }
  }

  s << "));";
  if (fields_.HasPayload()) {
    s << "builder->payload_ = std::move(payload);";
  }
  s << "return builder;";
  s << "}\n";
}

void PacketDef::GenBuilderParameterChecker(std::ostream& s) const {
  FieldList params_to_validate = GetParametersToValidate();

  // Skip writing this function if there is nothing to validate.
  if (params_to_validate.size() == 0) {
    return;
  }

  // Generate function arguments.
  s << "void CheckParameterValues(";
  for (int i = 0; i < params_to_validate.size(); i++) {
    params_to_validate[i]->GenBuilderParameter(s);
    if (i != params_to_validate.size() - 1) {
      s << ", ";
    }
  }
  s << ") {";

  // Check the parameters.
  for (const auto& field : params_to_validate) {
    field->GenParameterValidator(s);
  }
  s << "}\n";
}

void PacketDef::GenBuilderConstructor(std::ostream& s) const {
  s << name_ << "Builder(";

  // Generate the constructor parameters.
  auto params = GetParamList().GetFieldsWithoutTypes({
      PayloadField::kFieldType,
      BodyField::kFieldType,
  });
  for (int i = 0; i < params.size(); i++) {
    params[i]->GenBuilderParameter(s);
    if (i != params.size() - 1) {
      s << ", ";
    }
  }
  s << ") :";

  // Get the list of parent params to call the parent constructor with.
  FieldList parent_params;
  if (parent_ != nullptr) {
    // Pass parameters to the parent constructor
    s << parent_->name_ << "Builder(";
    parent_params = parent_->GetParamList().GetFieldsWithoutTypes({
        PayloadField::kFieldType,
        BodyField::kFieldType,
    });

    // Go through all the fields and replace constrained fields with fixed values
    // when calling the parent constructor.
    for (int i = 0; i < parent_params.size(); i++) {
      const auto& field = parent_params[i];
      const auto& constraint = parent_constraints_.find(field->GetName());
      if (constraint != parent_constraints_.end()) {
        if (field->GetFieldType() == ScalarField::kFieldType) {
          s << std::get<int64_t>(constraint->second);
        } else if (field->GetFieldType() == EnumField::kFieldType) {
          s << std::get<std::string>(constraint->second);
        } else {
          ERROR(field) << "Constraints on non enum/scalar fields should be impossible.";
        }

        s << "/* " << field->GetName() << "_ */";
      } else {
        s << field->GetName();
      }

      if (i != parent_params.size() - 1) {
        s << ", ";
      }
    }
    s << ") ";
  }

  // Build a list of parameters that excludes all parent parameters.
  FieldList saved_params;
  for (const auto& field : params) {
    if (parent_params.GetField(field->GetName()) == nullptr) {
      saved_params.AppendField(field);
    }
  }
  if (parent_ != nullptr && saved_params.size() > 0) {
    s << ",";
  }
  for (int i = 0; i < saved_params.size(); i++) {
    const auto& saved_param_name = saved_params[i]->GetName();
    s << saved_param_name << "_(" << saved_param_name << ")";
    if (i != saved_params.size() - 1) {
      s << ",";
    }
  }
  s << " {";

  FieldList params_to_validate = GetParametersToValidate();

  if (params_to_validate.size() > 0) {
    s << "CheckParameterValues(";
    for (int i = 0; i < params_to_validate.size(); i++) {
      s << params_to_validate[i]->GetName() << "_";
      if (i != params_to_validate.size() - 1) {
        s << ", ";
      }
    }
    s << ");";
  }

  s << "}\n";
}

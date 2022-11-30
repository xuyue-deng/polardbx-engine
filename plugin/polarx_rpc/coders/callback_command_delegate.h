//
// Created by zzy on 2022/8/31.
//

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <sys/types.h>
#include <vector>

#include "../global_defines.h"
#ifndef MYSQL8
#include "my_global.h"
#endif

#include "command_delegate.h"

namespace polarx_rpc {

class CcallbackCommandDelegate : public CcommandDelegate {
public:
  struct Field_value {
    Field_value();
    Field_value(const Field_value &other);
    explicit Field_value(const char *str, size_t length);

    explicit Field_value(const longlong &num, bool unsign = false);
    explicit Field_value(const decimal_t &decimal);
    explicit Field_value(const double num);
    explicit Field_value(const MYSQL_TIME &time);

    Field_value &operator=(const Field_value &other);

    ~Field_value();
    union {
      longlong v_long;
      double v_double;
      decimal_t v_decimal;
      MYSQL_TIME v_time;
      std::string *v_string;
    } value;
    bool is_unsigned;
    bool is_string;
  };

  struct Row_data {
    Row_data() = default;
    Row_data(const Row_data &other);
    Row_data &operator=(const Row_data &other);
    ~Row_data();
    void clear();
    std::vector<Field_value *> fields;

  private:
    void clone_fields(const Row_data &other);
  };

  using Start_row_callback = std::function<Row_data *()>;
  using End_row_callback = std::function<bool(Row_data *)>;

  CcallbackCommandDelegate();
  CcallbackCommandDelegate(Start_row_callback start_row,
                           End_row_callback end_row);

  void set_callbacks(Start_row_callback start_row, End_row_callback end_row);

  void reset() override;

  enum cs_text_or_binary representation() const override {
    return CS_TEXT_REPRESENTATION;
  }

private:
  Start_row_callback m_start_row;
  End_row_callback m_end_row;
  Row_data *m_current_row = nullptr;

private:
  int start_row() override;

  int end_row() override;

  void abort_row() override;

  ulong get_client_capabilities() override;

  /****** Getting data ******/
  int get_null() override;

  int get_integer(longlong value) override;

  int get_longlong(longlong value, uint32_t unsigned_flag) override;

  int get_decimal(const decimal_t *value) override;

  int get_double(double value, uint32_t decimals) override;

  int get_date(const MYSQL_TIME *value) override;

  int get_time(const MYSQL_TIME *value, uint32_t decimals) override;

  int get_datetime(const MYSQL_TIME *value, uint32_t decimals) override;

  int get_string(const char *const value, size_t length,
                 const CHARSET_INFO *const valuecs) override;
};

} // namespace polarx_rpc

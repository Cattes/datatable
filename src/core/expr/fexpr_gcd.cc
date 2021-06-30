#include "column/virtual.h"
#include "documentation.h"
#include "expr/fexpr_func.h"
#include "expr/eval_context.h"
#include "expr/workframe.h"
#include "python/xargs.h"
#include "stype.h"


namespace dt {
namespace expr {


template <typename T>
class Column_Gcd : public Virtual_ColumnImpl {
  private:
    Column acol_;
    Column bcol_;

  public:
    Column_Gcd(Column&& a, Column&& b)
      : Virtual_ColumnImpl(a.nrows(), a.stype()),
        acol_(std::move(a)), bcol_(std::move(b))
    {
      xassert(acol_.nrows() == bcol_.nrows());
      xassert(acol_.stype() == bcol_.stype());
      xassert(acol_.can_be_read_as<T>());
    }

    ColumnImpl* clone() const override {
      return new Column_Gcd(Column(acol_), Column(bcol_));
    }

    size_t n_children() const noexcept override { return 2; }
    const Column& child(size_t i) const override { return i==0? acol_ : bcol_; }

    bool get_element(size_t i, T* out) const override {
      T a, b;
      bool avalid = acol_.get_element(i, &a);
      bool bvalid = bcol_.get_element(i, &b);
      if (avalid && bvalid) {
        while (b) {
          T tmp = b;
          b = a % b;
          a = tmp;
        }
        *out = a;
        return true;
      }
      return false;
    }
};

class FExpr_Gcd : public FExpr_Func {
  private:
    ptrExpr a_;
    ptrExpr b_;

  public:
    FExpr_Gcd(ptrExpr&& a, ptrExpr&& b)
      : a_(std::move(a)), b_(std::move(b)) {}


    std::string repr() const override {
      std::string out = "gcd(";
      out += a_->repr();
      out += ", ";
      out += b_->repr();
      out += ')';
      return out;
    }

    Workframe evaluate_n(EvalContext& ctx) const override {
      Workframe awf = a_->evaluate_n(ctx);
      Workframe bwf = b_->evaluate_n(ctx);
      if (awf.ncols() == 1) awf.repeat_column(bwf.ncols());
      if (bwf.ncols() == 1) bwf.repeat_column(awf.ncols());
      if (awf.ncols() != bwf.ncols()) {
        throw TypeError() << "Incompatible number of columns in " << repr()
            << ": the first argument has " << awf.ncols() << ", while the "
            << "second has " << bwf.ncols();
      }
      awf.sync_grouping_mode(bwf);

      auto gmode = awf.get_grouping_mode();
      Workframe outputs(ctx);
      for (size_t i = 0; i < awf.ncols(); ++i) {
        Column rescol = evaluate1(awf.retrieve_column(i),
                                  bwf.retrieve_column(i));
        outputs.add_column(std::move(rescol), std::string(), gmode);
      }
      return outputs;
    }

    Column evaluate1(Column&& a, Column&& b) const {
      SType stype1 = a.stype();
      SType stype2 = b.stype();
      SType stype0 = common_stype(stype1, stype2);
      switch (stype0) {
        case SType::BOOL:
        case SType::INT8:
        case SType::INT16:
        case SType::INT32: return make<int32_t>(std::move(a), std::move(b), SType::INT32);
        case SType::INT64: return make<int64_t>(std::move(a), std::move(b), SType::INT64);
        default:
            throw TypeError() << "Invalid columns of types " << stype1 << " and "
                << stype2 << " in " << repr();
      }
    }

    template <typename T>
    Column make(Column&& a, Column&& b, SType stype0) const {
      a.cast_inplace(stype0);
      b.cast_inplace(stype0);
      return Column(new Column_Gcd<T>(std::move(a), std::move(b)));
    }
};


static py::oobj py_gcd(const py::XArgs& args) {
  auto a = args[0].to_oobj();
  auto b = args[1].to_oobj();
  return PyFExpr::make(new FExpr_Gcd(as_fexpr(a), as_fexpr(b)));
}

DECLARE_PYFN(&py_gcd)
    ->name("gcd")
    ->docs(dt::doc_dt_gcd)
    ->arg_names({"a", "b"})
    ->n_positional_args(2)
    ->n_required_args(2);

}}  // namespace dt::expr::
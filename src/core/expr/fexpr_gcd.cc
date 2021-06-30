#include "column/virtual.h"
#include "documentation.h"
#include "expr/fexpr_func.h"
#include "expr/eval_context.h"
#include "expr/workframe.h"
#include "python/xargs.h"
#include "stype.h"


namespace dt {
namespace expr {

// Template for Class allows the compiler to create type specific classes for the different allowed types
// This thing does the actual work, Although I do not understand the greatest common denominator get_element function.
template <typename T>
class Column_Gcd : public Virtual_ColumnImpl {
    // private properties acol_ and bcol_ for class input
  private:
    Column acol_;
    Column bcol_;

    // a lot more is public
  public:
    // first some generic asserts that the columns are ok
    Column_Gcd(Column&& a, Column&& b)
      : Virtual_ColumnImpl(a.nrows(), a.stype()),
        acol_(std::move(a)), bcol_(std::move(b))
    {
      xassert(acol_.nrows() == bcol_.nrows());
      xassert(acol_.stype() == bcol_.stype());
      xassert(acol_.can_be_read_as<T>());
    }
    // Then some generic clone() method that creates a new instance
    ColumnImpl* clone() const override {
      return new Column_Gcd(Column(acol_), Column(bcol_));
    }

    // No idea what that does. some error code handling?
    size_t n_children() const noexcept override { return 2; }
    const Column& child(size_t i) const override { return i==0? acol_ : bcol_; }

    // A get element function than returns bool?
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

// Here we have the actual GcD F Expression that will interface with python (I think)
class FExpr_Gcd : public FExpr_Func {
    // Again only the inputs as private
  private:
    ptrExpr a_;
    ptrExpr b_;

  public:
    // a lot of public, starting with no idea what that is, I guess an init?
    FExpr_Gcd(ptrExpr&& a, ptrExpr&& b)
      : a_(std::move(a)), b_(std::move(b)) {}

    // function call representation, used for logging i think. But the breaket precendence is supposedly important
    // So no idea if that really is only for printing
    std::string repr() const override {
      std::string out = "gcd(";
      out += a_->repr();
      out += ", ";
      out += b_->repr();
      out += ')';
      return out;
    }

    // Now the have the evaluate normal `Workframe` creation given the EvalContext ctx.
    Workframe evaluate_n(EvalContext& ctx) const override {
        // First we extract a and b into Workframe awf and bwf from the context
      Workframe awf = a_->evaluate_n(ctx);
      Workframe bwf = b_->evaluate_n(ctx);
      // Some input preparing. Got to look up hat methods are implemented on the awf class
      if (awf.ncols() == 1) awf.repeat_column(bwf.ncols());
      if (bwf.ncols() == 1) bwf.repeat_column(awf.ncols());
      if (awf.ncols() != bwf.ncols()) {
        throw TypeError() << "Incompatible number of columns in " << repr()
            << ": the first argument has " << awf.ncols() << ", while the "
            << "second has " << bwf.ncols();
      }
      // no idea what that does
      awf.sync_grouping_mode(bwf);

      // no idea here either
      auto gmode = awf.get_grouping_mode();

      // define an Workframe output object and then loop through the a work frame
      // always use evaluate1 row of data
      // but why is awf.ncols used for moving thorugh awf and bwf?
      Workframe outputs(ctx);
      for (size_t i = 0; i < awf.ncols(); ++i) {
        Column rescol = evaluate1(awf.retrieve_column(i),
                                  bwf.retrieve_column(i));
        outputs.add_column(std::move(rescol), std::string(), gmode);
      }
      return outputs;
    }

    // Transform column a and b into an output using the make function for the different column input types
    // `make` itself uses the Column_Gcd<T> Template Class to return the output
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

    // Transform column a and b into the given stype0 and create the ColumnGcd class and call the Column function with it?
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
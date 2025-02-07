/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file src/relay/analysis/device_domains.h
 * \brief Unification domain for the device planner.
 */

#ifndef TVM_RELAY_TRANSFORMS_DEVICE_DOMAINS_H_
#define TVM_RELAY_TRANSFORMS_DEVICE_DOMAINS_H_

#include <dlpack/dlpack.h>
#include <tvm/relay/expr.h>
#include <tvm/relay/type.h>
#include <tvm/runtime/ndarray.h>
#include <tvm/target/compilation_config.h>
#include <tvm/target/se_scope.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tvm {
namespace relay {
namespace transform {

class DeviceDomain;
using DeviceDomainPtr = std::shared_ptr<DeviceDomain>;
class DeviceDomains;

/*!
 * \brief Represents the domain over which we collect equality constraints.
 *
 * \code
 *   D ::= ?x?                  -- first order, free
 *       | <se_scope>           -- first order, bound to specific device and memory scope
 *       | fn(D1, ..., Dn):Dr   -- higher order
 * \endcode
 *
 * We require a function value to be on the same device as its result. To support that we need
 * a notion of the 'result domain' of a domain:
 * \code
 *   result_domain(?x?)                = ?x?
 *   result_domain(<se_scope>)         = <se_scope>
 *   result_domain(fn(D1, ..., Dn):Dr) = result_domain(Dr)
 * \endcode
 */
class DeviceDomain {
 public:
  /*!
   * \brief Constructs a first-order domain for \p se_scope, which may be
   * fully free (ie se_scope is unconstrained), partially  free (ie se_scope has at least on
   * of its target, device id or memory scopes known), or fully fixed (ie se_scope has its target,
   * device id and memory scopes set).
   *
   * CAUTION: Use DeviceDomains::MakeFirstOrderDomain instead of this ctor.
   */
  explicit DeviceDomain(SEScope se_scope) : se_scope_(std::move(se_scope)) {}

  /*!
   * \brief Constructs a higher-order domain, where \p args_and_result contain the
   * function argument and result domains in order.
   *
   * CAUTION: Use DeviceDomains::MakeHigherOrderDomain instead of this ctor.
   */
  explicit DeviceDomain(std::vector<DeviceDomainPtr> args_and_result)
      : se_scope_(SEScope::FullyUnconstrained()), args_and_result_(std::move(args_and_result)) {}

  bool is_higher_order() const { return !args_and_result_.empty(); }

  SEScope first_order_se_scope() const {
    ICHECK(args_and_result_.empty()) << "expecting domain to be first-order";
    return se_scope_;
  }

  size_t function_arity() const {
    ICHECK(!args_and_result_.empty()) << "expecting domain to be higher-order";
    return args_and_result_.size() - 1UL;
  }

  DeviceDomainPtr function_param(size_t i) const {
    ICHECK(!args_and_result_.empty()) << "expecting domain to be higher-order";
    ICHECK_LT(i + 1, args_and_result_.size()) << "parameter index is out of range";
    return args_and_result_[i];
  }

  DeviceDomainPtr function_result() const {
    ICHECK(!args_and_result_.empty());
    return args_and_result_.back();
  }

 private:
  /*!
   * \brief If this is a function domain then always fully unconstrained. Otherwise will be
   * fully unconstrained (the domain is still completely free), partially constrained
   * (for example, the \p target and \p device_type are constrained but the \p virtual_device_id and
   * \p memory_scope are still unconstrained), or fully constrained (everything is known).
   */
  const SEScope se_scope_;

  /*!
   * \brief If this is a function domain then the sub-domains for each of the function's
   * arguments, and the domain for its result. Otherwise empty.
   */
  const std::vector<DeviceDomainPtr> args_and_result_;

  friend class DeviceDomains;
};

/*!
 * \brief Tracks the device domains for a set of expressions w.r.t. an equivalence relation
 * built up by calls to \p UnifyOrNull.
 */
class DeviceDomains {
 public:
  explicit DeviceDomains(CompilationConfig config);

  const CompilationConfig& config() const { return config_; }

  /*!
   * \brief Returns the domain representing \p se_scope. If \p se_scope is fully constrained
   * then the domain will be unique that \p se_scope.
   */
  DeviceDomainPtr MakeFirstOrderDomain(const SEScope& se_scope);

  /*!
   * \brief Returns a higher-order domain with \p args_and_results.
   */
  DeviceDomainPtr MakeHigherOrderDomain(std::vector<DeviceDomainPtr> arg_and_results) {
    return std::make_shared<DeviceDomain>(std::move(arg_and_results));
  }

  /*!
   * \brief Returns a domain appropriate for \p type who's result domain is bound to \p se_scope.
   * If \p type is a function then all parameter domains will be completely free. It is valid for
   * \p se_scope to be fully unconstrained.
   */
  DeviceDomainPtr MakeDomain(const Type& type, const SEScope& se_scope);

  /*!
   * \brief Returns a domain with the given result appropriate \p non_canonical_se_scope,
   * which cannot be fully unconstrained. We first canonicalize the scope to unsure it has
   * a target and is unique.
   */
  DeviceDomainPtr ForSEScope(const Type& type, const SEScope& non_canonical_se_scope);

  /*! \brief Returns a free domain appropriate for \p type. */
  DeviceDomainPtr Free(const Type& type) { return MakeDomain(type, SEScope::FullyUnconstrained()); }

  /*! \brief Returns the domain representing the equivalence class containing \p domain. */
  DeviceDomainPtr Lookup(DeviceDomainPtr domain);

  /*!
   * \brief Returns the most constrained domain which agrees with both \p lhs and \p rhs. Returns
   * null if no such domain exists, ie some first-order component of \p lhs is constrained
   * differently than the corresponding component of \p rhs.
   */
  DeviceDomainPtr JoinOrNull(const DeviceDomainPtr& lhs, const DeviceDomainPtr& rhs);

  /*!
   * \brief Unifies \p lhs and \p rhs, returning the most-bound of the two. Returns null if
   * \p lhs and \p rhs are not unifiable.
   */
  // TODO(mbs): I don't think we need an occurs check since the program is well-typed, but
  // given we have refs to functions I'm prepared to be surprised.
  DeviceDomainPtr UnifyOrNull(DeviceDomainPtr lhs, DeviceDomainPtr rhs);

  /*
   * \brief Force all domains in \p higher_order_domain to unify with \p first_order_domain.
   * This can be used to handle functions within tuples, references and ADTs since we don't
   * attempt to track anything beyond 'the device' for expressions of those first-order types.
   *
   * Returns false if any unification fails.
   */
  bool CollapseOrFalse(const DeviceDomainPtr& first_order_domain,
                       const DeviceDomainPtr& higher_order_domain);

  /*!
   * \brief Unifies \p lhs_first_order and \p rhs_maybe_higher_order. If \p rhs_maybe_higher_order
   * is indeed higher-order, require all of its arguments and result to unify with
   * \p lhs_first_order. Otherwise same as \p Unify. Returns false if unification is not possible.
   *
   * In an expression such as:
   * \code
   * (fn(...) {...}, ...).0
   * \endcode
   * we need to force all the devices of the inner function to be the same as the device for the
   * overall tuple since the device domain does not understand tuples. Similarly for references
   * and ADTs.
   */
  bool UnifyCollapsedOrFalse(const DeviceDomainPtr& lhs_first_order,
                             const DeviceDomainPtr& rhs_maybe_higher_order);

  /*! \brief Returns true if a domain is known for \p expr. */
  bool contains(const Expr& expr) const { return expr_to_domain_.count(expr.get()); }

  /*! \brief Returns the domain representing \p expr. */
  DeviceDomainPtr DomainFor(const Expr& expr);

  /*!
   * \brief Returns the domain representing the callee (ie 'op') in \p call expression. If the
   * callee is a primitive or special operation we handle it specially. Otherwise defers to \p
   * DomainFor(call->op).
   *
   * This special handling is needed:
   * - To handle the "on_device" and "device_copy" ops which constrain devices to the given
   * devices.
   * - To handle some special ops which constrain devices to the CPU.
   * - To allow the same primitive to be called on different devices at different call sites.
   * Since each call to the op can have a different domain we index the ops by the call expression
   * rather than the op itself.
   */
  DeviceDomainPtr DomainForCallee(const Call& call);

  /*!
   * \brief Unifies the domains for expressions \p lhs and \p rhs.
   *
   * Aborts if unification fails.
   */
  void UnifyExprExact(const Expr& lhs, const Expr& rhs);

  /*!
   * \brief Unifies the domain for \p expr with \p expected_domain.
   *
   * Aborts if unification fails.
   */
  void UnifyExprExact(const Expr& expr, const DeviceDomainPtr& expected_domain);

  /*!
   * \brief Unifies the domain for \p expr with \p expected_domain.
   * If \p expected_domain is higher-order but \p expr is first-order, require all arguments
   * and the result of \p expected_domain to have the same domain as for \p expr.
   *
   * Aborts if unification fails.
   */
  void UnifyExprCollapsed(const Expr& expr_first_order,
                          const DeviceDomainPtr& expected_domain_maybe_higher_order);

  /*! \brief Returns true if \p domain is fully constrainted. */
  bool IsFullyConstrained(DeviceDomainPtr domain);

  /*! \brief Force all \p SEScopes in \p domain to default to \p default_se_scope. */
  void SetDefault(DeviceDomainPtr domain, const SEScope& default_se_scope);

  /*!
   * \brief If \p domain is higher-order default it's result domain to \p default_se_scope.
   * Then force all remaining \p SEScopes to the result domain (freshly defaulted or original).
   * If \p domain is first-order same as \p SetDefault.
   */
  void SetResultDefaultThenParams(const DeviceDomainPtr& domain_maybe_higher_order,
                                  const SEScope& default_se_scope);

  /*!
   * \brief Returns the result domain for \p domain (see defn in DeviceDomain comment).
   */
  DeviceDomainPtr ResultDomain(DeviceDomainPtr domain);

  /*!
   * \brief Returns the result \p SEScope (possibly unconstrained) for \p domain
   * (see defn in DeviceDomain comment).
   */
  SEScope ResultSEScope(const DeviceDomainPtr& domain) {
    return ResultDomain(domain)->first_order_se_scope();
  }

  /*! \brief Returns one-line description of \p domain for debugging. */
  std::string ToString(DeviceDomainPtr domain);

  /*! \brief Returns description of entire system of constraints for debugging */
  std::string ToString();

 private:
  /*! \brief Intrinsics we need to handle specially. */
  const Op& alloc_storage_op = Op::Get("memory.alloc_storage");
  const Op& alloc_tensor_op = Op::Get("memory.alloc_tensor");
  const Op& shape_of_op = Op::Get("vm.shape_of");
  const Op& invoke_tvm_op = Op::Get("vm.invoke_tvm_op");
  const Op& reshape_tensor_op = Op::Get("vm.reshape_tensor");

  CompilationConfig config_;

  /*!
   * \brief The domain for first-order expressions of non-tensor type, such as shapes and
   * buffer dimensions. Generally this will be a CPU.
   */
  DeviceDomainPtr host_domain_;

  /*! \brief Maps expressions to their domains as determined during analysis. */
  std::unordered_map<const ExprNode*, DeviceDomainPtr> expr_to_domain_;

  /*!
   * \brief Maps call expressions to the domains for their callee where the callee is a primitive.
   */
  std::unordered_map<const CallNode*, DeviceDomainPtr> call_to_callee_domain_;

  /*! \brief Maps device domains to their equivalent domains as determined during unification. */
  std::unordered_map<DeviceDomainPtr, DeviceDomainPtr> domain_to_equiv_;

  /*!
   * \brief Maps fully constrained \p SEScopes to their corresponding domains. By sharing those
   * domains we can ensure:
   *
   * \code
   * domain0 != domain1 && domain0 fully constrained && domain1 fully constrained
   *   ==> domain0 and domain1 are incompatible
   * \endcode
   */
  std::unordered_map<SEScope, DeviceDomainPtr, runtime::ObjectPtrHash, runtime::ObjectPtrEqual>
      fully_constrained_se_scope_to_domain_;
};

}  // namespace transform
}  // namespace relay
}  // namespace tvm

#endif  // TVM_RELAY_TRANSFORMS_DEVICE_DOMAINS_H_

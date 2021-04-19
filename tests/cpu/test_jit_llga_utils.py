import os
import copy

import torch
from torch.testing._internal.jit_utils import JitTestCase, warmup_backward, \
    get_execution_plan
from torch.testing._internal.common_utils import freeze_rng_state, run_tests, \
    get_function_arglist, load_tests, repeat_test_for_types, TemporaryFileName

from torch.jit._recursive import wrap_cpp_module

import intel_pytorch_extension as ipex

LLGA_FUSION_GROUP = 'ipex::LlgaFusionGroup'

# disable PyTorch jit profiling
torch._C._jit_set_profiling_mode(False)
torch._C._jit_set_profiling_executor(False)

# disbale ipex jit optimization for fp32 and bf16 path
ipex.core.disable_jit_opt()

def all_backward_graphs(module):
    ge_state = module.get_debug_state()
    fwd_plan = get_execution_plan(ge_state)
    executors = fwd_plan.code.grad_executor_states()
    assert len(executors), 'No backward graph found in the module'
    grad_executor = executors[0]
    bwd_plans = list(grad_executor.execution_plans.values())
    return [p.graph.copy() for p in bwd_plans]


def backward_graph(module):
    graphs = all_backward_graphs(module)
    assert len(graphs), 'Warm up the module before calling backward_graph'
    return graphs[0]


def freeze(model):
    return wrap_cpp_module(torch._C._freeze_module(model._c, preserveParameters=True))


class JitLlgaTestCase(JitTestCase):
    def checkScript(self, m, x):
        requires_grad = any(t.requires_grad for t in x)
        with torch.set_grad_enabled(requires_grad):
            ref = m(*x)
            scripted = torch.jit.script(m)
            y = scripted(*x)
            self.assertEqual(y, ref)
            graph = scripted.graph_for(*x)
        return scripted, graph

    def checkTrace(self, m, x, *args, **kwargs):
        grad = any(t.requires_grad for t in x)
        with torch.set_grad_enabled(grad), \
                torch._jit_internal._disable_emit_hooks():
            traced = super().checkTrace(m, x, inputs_require_grads=grad)
            fwd_graph = traced.graph_for(*x)
        if grad:
            warmup_backward(traced(*x).sum())
            return traced, fwd_graph, backward_graph(traced)
        else:
            return traced, fwd_graph

    def assertFused(self, graph, fused_patterns):
        for pat in fused_patterns:
            self.assertGraphContainsExactly(graph, pat, 0)

    def checkQuantizeTrace(self, model, x, atol=1e-3, rtol=1e-2, folding=False):
        model.eval()
        with torch.no_grad():
            # fold conv bn
            if folding:  
                model = ipex.fx.conv_bn_fuse(model)

            # do calibration
            conf = ipex.AmpConf(torch.int8)
            with torch.no_grad():
                with ipex.amp.calibrate():
                    y = model(x)
            conf.save('configure.json')

            conf = ipex.AmpConf(torch.int8, 'configure.json')

            # jit trace to insert quant/dequant
            with ipex.amp.autocast(enabled=True, configure=conf):
                model = torch.jit.trace(model, x, check_trace=False)

            fp32_model_with_quant_dequant = copy.deepcopy(model)


            # freeze the module
            model = freeze(model)

            # apply llga optimization pass
            ipex.core._jit_llga_fuser(model.graph)

            os.remove('configure.json')

            print("before cpu")
            y = fp32_model_with_quant_dequant(x)
            print("after cpu")
            y_llga = model(x)
            print("After llga")

            self.assertEqual(y, y_llga, atol=atol, rtol=rtol)
            print(model.graph)
            return model.graph
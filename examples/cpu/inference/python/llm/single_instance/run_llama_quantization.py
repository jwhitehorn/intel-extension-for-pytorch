import argparse
import time
import json
import pathlib

from datasets import load_dataset
from transformers import LlamaForCausalLM, LlamaTokenizer, AutoConfig

import torch
from torch.nn.functional import pad
from torch.utils.data import DataLoader

import intel_extension_for_pytorch as ipex


parser = argparse.ArgumentParser("LLaMA generation script (int8 path)", add_help=False)
parser.add_argument(
    "-m", "--model-id", default=None, type=str, required=True, help="your llama model"
)
parser.add_argument(
    "--max-new-tokens", default=32, type=int, help="output max new tokens"
)
parser.add_argument("--dataset", nargs="?", default="NeelNanda/pile-10k")
parser.add_argument("--split", nargs="?", default="validation", const="validation")
parser.add_argument("--output-dir", nargs="?", default="./saved_results")
parser.add_argument("--ipex-smooth-quant", action="store_true")
parser.add_argument(
    "--ipex-weight-only-quantization",
    action="store_true",
    help="use ipex weight-only quantization",
)
parser.add_argument("--int8", action="store_true")
parser.add_argument(
    "--int8-bf16-mixed",
    action="store_true",
    help="by default it is int8-fp32 mixed, to enable int8 mixed amp bf16 (work on platforms like SPR)",
)
parser.add_argument("--quantized-model-path", default="./saved_results/best_model.pt")
parser.add_argument("--benchmark", action="store_true")
parser.add_argument("--input-tokens", default="32", type=str)
parser.add_argument("--prompt", default=None, type=str)
parser.add_argument("--num-iter", default=100, type=int, help="num iter")
parser.add_argument("--num-warmup", default=10, type=int, help="num warmup")
parser.add_argument("--batch-size", default=1, type=int, help="batch size")
parser.add_argument("--alpha", default=0.8, type=float, help="alpha value for smoothquant")
parser.add_argument("--token-latency", action="store_true")
parser.add_argument("--greedy", action="store_true")
parser.add_argument("--profile", action="store_true")
parser.add_argument(
    "--lowp-mode",
    choices=["AUTO", "BF16", "FP32", "INT8", "FP16"],
    default="AUTO",
    type=str,
    help="low precision mode for weight only quantization. "
         "It indicates data type for computation for speedup at the cost "
         "of accuracy. Unrelated to activation or weight data type."
         "It is not supported yet to use lowp_mode=INT8 for INT8 weight, "
         "falling back to lowp_mode=BF16 implicitly in this case."
         "If set to AUTO, lowp_mode is determined by weight data type: "
         "lowp_mode=BF16 is used for INT8 weight "
         "and lowp_mode=INT8 used for INT4 weight",
)
parser.add_argument(
    "--weight-dtype",
    choices=["INT8", "INT4"],
    default="INT8",
    type=str,
    help="weight data type for weight only quantization. Unrelated to activation"
         " data type or lowp-mode. If `--low-precision-checkpoint` is given, weight"
         " data type is always INT4 and this argument is not needed.",
)
parser.add_argument(
    "--low-precision-checkpoint",
    default="",
    type=str,
    help="Low precision checkpoint file generated by calibration, such as GPTQ. It contains"
         " modified weights, scales, zero points, etc. For better accuracy of weight only"
         " quantization with INT4 weight.",
)
args = parser.parse_args()


# disable
try:
    ipex._C.disable_jit_linear_repack()
except Exception:
    pass

# amp autocast
if args.int8_bf16_mixed:
    amp_enabled = True
    amp_dtype = torch.bfloat16
else:
    amp_enabled = False
    amp_dtype = torch.float32


num_beams = 1 if args.greedy else 4
generate_kwargs = dict(do_sample=False, temperature=0.9, num_beams=num_beams)


# load model
config = AutoConfig.from_pretrained(args.model_id, torchscript=True)
if not hasattr(config, "text_max_length") and args.prompt is None:
    config.text_max_length = int(args.input_tokens) + int(args.max_new_tokens)

if args.benchmark:
    try:
        with ipex._IPEXOnDevice(dtype=torch.float, device="meta"):
            user_model = LlamaForCausalLM._from_config(config)
    except (RuntimeError, AttributeError):
        user_model = LlamaForCausalLM.from_pretrained(
            args.model_id, config=config, low_cpu_mem_usage=True, torch_dtype=torch.half
        )
else:
    user_model = LlamaForCausalLM.from_pretrained(
        args.model_id, config=config, low_cpu_mem_usage=True, torch_dtype=torch.float
    )

tokenizer = LlamaTokenizer.from_pretrained(args.model_id)
print("Data type of the model:", user_model.dtype)

# dummy past key value
beam_idx_tmp = torch.zeros(
    (2048, int(args.batch_size * num_beams)), dtype=torch.long
).contiguous()
global_past_key_value = [
    (
        torch.zeros(1, 0, 0, 1, dtype=torch.long).contiguous(),
        torch.zeros(
            [
                1,
                user_model.config.num_attention_heads,
                1,
                int(
                    user_model.config.hidden_size
                    / user_model.config.num_attention_heads
                ),
            ]
        ).contiguous(),
        torch.zeros(
            [
                1,
                user_model.config.num_attention_heads,
                1,
                int(
                    user_model.config.hidden_size
                    / user_model.config.num_attention_heads
                ),
            ]
        ).contiguous(),
        beam_idx_tmp,
    )
    for i in range(user_model.config.num_hidden_layers)
]


if args.ipex_smooth_quant:

    class Evaluator:
        def __init__(self, dataset, tokenizer, batch_size=1, pad_val=1, pad_max=512):
            self.dataset = dataset
            self.tokenizer = tokenizer
            self.batch_size = batch_size
            self.pad_val = pad_val
            self.pad_max = pad_max

            # tokenize the dataset
            self.dataset = self.dataset.map(self.tokenize_function, batched=True)
            self.dataset.set_format(type="torch", columns=["input_ids"])

        @torch.no_grad()
        def tokenize_function(self, examples):
            if "prompt" in examples:
                example = self.tokenizer(examples["prompt"])
            elif "text" in examples:
                example = self.tokenizer(examples["text"])
            elif "code" in examples:
                example = self.tokenizer(examples["code"])
            return example

        @torch.no_grad()
        def collate_batch(self, batch):
            position_ids_padded = []
            input_ids_padded = []
            last_ind = []
            attention_mask_padded = []
            for text in batch:
                input_ids = text["input_ids"]
                input_ids = (
                    input_ids[: int(self.pad_max)]
                    if len(input_ids) > int(self.pad_max)
                    else input_ids
                )
                last_ind.append(input_ids.shape[0] - 1)
                attention_mask = torch.ones(len(input_ids))
                position_ids = torch.arange(len(input_ids))
                input_ids_padded.append(input_ids)
                attention_mask_padded.append(attention_mask)
                position_ids_padded.append(position_ids)
            return (
                (
                    torch.vstack(input_ids_padded),
                    torch.vstack(attention_mask_padded),
                    torch.vstack(position_ids_padded),
                    tuple(global_past_key_value),
                ),
                torch.tensor(last_ind),
            )

    calib_dataset = load_dataset(args.dataset, split="train")
    user_model.eval()
    calib_evaluator = Evaluator(calib_dataset, tokenizer, args.batch_size)
    calib_dataloader = DataLoader(
        calib_evaluator.dataset,
        batch_size=args.batch_size,
        shuffle=False,
        collate_fn=calib_evaluator.collate_batch,
    )

    def calib_func(prepared_model):
        for i, (
            (input_ids, attention_mask, position_ids, past_key_values),
            last_ind,
        ) in enumerate(calib_dataloader):
            if i == 512:
                break
            prepared_model(
                input_ids,
                attention_mask=attention_mask,
                position_ids=position_ids,
                past_key_values=past_key_values,
            )

    example_inputs = None
    for i, (
        (input_ids, attention_mask, position_ids, past_key_values),
        last_ind,
    ) in enumerate(calib_dataloader):
        example_inputs = (input_ids, attention_mask, position_ids, past_key_values)
        break
    from intel_extension_for_pytorch.quantization import prepare, convert

    qconfig = ipex.quantization.get_smooth_quant_qconfig_mapping(alpha=args.alpha)
    user_model = ipex.optimize_transformers(
        user_model.eval(),
        dtype=amp_dtype,
        quantization_config=qconfig,
        inplace=True,
        deployment_mode=False,
    )
    prepared_model = prepare(
        user_model.eval(), qconfig, example_inputs=example_inputs, inplace=True
    )
    with torch.no_grad():
        for i, (
            (input_ids, attention_mask, position_ids, past_key_values),
            last_ind,
        ) in enumerate(calib_dataloader):
            if i == 512:
                break
            prepared_model(
                input_ids,
                position_ids=position_ids,
                attention_mask=attention_mask,
                past_key_values=past_key_values,
            )
    with torch.no_grad(), torch.cpu.amp.autocast(
        enabled=amp_enabled,
    ):
        convert_model = convert(prepared_model.eval(), inplace=True).eval()
        self_jit = torch.jit.trace(convert_model.eval(), example_inputs, strict=False)
        self_jit = torch.jit.freeze(self_jit.eval())
        pathlib.Path(args.output_dir).mkdir(parents=True, exist_ok=True)
        self_jit.save(args.output_dir + "/best_model.pt")
elif args.ipex_weight_only_quantization:
    weight_dtype = torch.quint4x2 if args.weight_dtype == "INT4" else torch.qint8

    if args.lowp_mode == "INT8":
        lowp_mode = ipex.quantization.WoqLowpMode.INT8
    elif args.lowp_mode == "FP32":
        lowp_mode = ipex.quantization.WoqLowpMode.NONE
    elif args.lowp_mode == "FP16":
        lowp_mode = ipex.quantization.WoqLowpMode.FP16
    elif args.lowp_mode == "BF16":
        lowp_mode = ipex.quantization.WoqLowpMode.BF16
    else:  # AUTO
        if args.low_precision_checkpoint != "" or weight_dtype == torch.quint4x2:
            lowp_mode = ipex.quantization.WoqLowpMode.INT8
        else:
            lowp_mode = ipex.quantization.WoqLowpMode.BF16

    qconfig = ipex.quantization.get_weight_only_quant_qconfig_mapping(
        weight_dtype=weight_dtype, lowp_mode=lowp_mode
    )
    if args.low_precision_checkpoint != "":
        low_precision_checkpoint = torch.load(args.low_precision_checkpoint)
    else:
        low_precision_checkpoint = None
    user_model = ipex.optimize_transformers(
        user_model.eval(),
        dtype=amp_dtype,
        quantization_config=qconfig,
        inplace=True,
        low_precision_checkpoint=low_precision_checkpoint,
        deployment_mode=False,
    )
    example_inputs = None
    input_ids = torch.ones(32).to(torch.long)
    attention_mask = torch.ones(len(input_ids))
    position_ids = torch.arange(len(input_ids))
    example_inputs = (
        input_ids.unsqueeze(0),
        attention_mask.unsqueeze(0),
        position_ids.unsqueeze(0),
        tuple(global_past_key_value),
    )
    with torch.no_grad(), torch.cpu.amp.autocast(
        enabled=amp_enabled,
    ):
        self_jit = torch.jit.trace(user_model.eval(), example_inputs, strict=False)
        self_jit = torch.jit.freeze(self_jit.eval())
        pathlib.Path(args.output_dir).mkdir(parents=True, exist_ok=True)
        self_jit.save(args.output_dir + "/best_model.pt")


if args.benchmark:
    torch._C._jit_set_texpr_fuser_enabled(False)
    qconfig = ipex.quantization.default_static_qconfig_mapping
    user_model = ipex.optimize_transformers(
        user_model.eval(),
        dtype=torch.float,
        inplace=True,
        quantization_config=qconfig,
        deployment_mode=False,
    )
    if not hasattr(user_model, "trace_graph"):
        print("load_quantized_model")
        self_jit = torch.jit.load(args.quantized_model_path)
        self_jit = torch.jit.freeze(self_jit.eval())
        ipex._set_optimized_model_for_generation(user_model, optimized_model=self_jit)

    # input prompt
    current_path = pathlib.Path(__file__).parent.resolve()
    with open(str(current_path) + "/prompt.json") as f:
        prompt_pool = json.load(f)
    if args.prompt is not None:
        prompt = args.prompt
    elif int(args.input_tokens) > 8192:
        prompt = prompt_pool["llama"]["8192"] * int(int(args.input_tokens) / 8192)
    elif args.input_tokens in prompt_pool["llama"]:
        prompt = prompt_pool["llama"][args.input_tokens]
    else:
        raise SystemExit("[ERROR] Plese use --prompt if want to use custom input.")

    input_size = tokenizer(prompt, return_tensors="pt").input_ids.size(dim=1)
    print("---- Prompt size:", input_size)
    if args.token_latency:
        if not hasattr(user_model.config, "token_latency"):
            user_model.config.token_latency = True

    total_time = 0.0
    num_iter = args.num_iter
    num_warmup = args.num_warmup
    prompt = [prompt] * args.batch_size
    total_list = []
    with torch.inference_mode(), torch.no_grad(), torch.cpu.amp.autocast(
        enabled=amp_enabled
    ):
        for i in range(num_iter):
            tic = time.time()
            input_ids = tokenizer(prompt, return_tensors="pt").input_ids
            output = user_model.generate(
                input_ids, max_new_tokens=args.max_new_tokens, **generate_kwargs
            )
            gen_ids = output[0] if args.token_latency else output
            gen_text = tokenizer.batch_decode(gen_ids, skip_special_tokens=True)
            toc = time.time()
            input_tokens_lengths = [x.shape[0] for x in input_ids]
            output_tokens_lengths = [x.shape[0] for x in gen_ids]
            total_new_tokens = [
                o - i if user_model.config.model_type != "t5" else o
                for i, o in zip(input_tokens_lengths, output_tokens_lengths)
            ]
            print(gen_text, total_new_tokens, flush=True)
            print("Iteration: %d, Time: %.6f sec" % (i, toc - tic), flush=True)
            if i >= num_warmup:
                total_time += toc - tic
                if args.token_latency:
                    total_list.append(output[1])

    if args.profile:

        def trace_handler(prof):
            print(
                prof.key_averages().table(sort_by="self_cpu_time_total", row_limit=-1)
            )

        with torch.profiler.profile(
            activities=[torch.profiler.ProfilerActivity.CPU],
            schedule=torch.profiler.schedule(wait=1, warmup=3, active=1),
            on_trace_ready=trace_handler,
        ) as prof:
            for i in range(5):
                input_ids = tokenizer(prompt, return_tensors="pt").input_ids
                output = user_model.generate(
                    input_ids, max_new_tokens=args.max_new_tokens, **generate_kwargs
                )
                gen_ids = output[0] if args.token_latency else output
                gen_text = tokenizer.batch_decode(gen_ids, skip_special_tokens=True)
                prof.step()

    print("\n", "-" * 10, "Summary:", "-" * 10)
    latency = total_time / (num_iter - num_warmup)
    print("Inference latency: %.3f sec." % latency)
    if args.token_latency:
        import numpy as np
        from itertools import chain

        first_latency = np.mean([x[0] for x in total_list])
        average_2n = list(chain(*[x[1:] for x in total_list]))
        average_2n.sort()
        average_2n_latency = np.mean(average_2n)
        p90_latency = average_2n[int(len(average_2n) * 0.9)]
        p99_latency = average_2n[int(len(average_2n) * 0.99)]
        print("First token average latency: %.3f sec." % first_latency)
        print("Average 2... latency: %.3f sec." % average_2n_latency)
        print("P90 2... latency: %.3f sec." % p90_latency)
        print("P99 2... latency: %.3f sec." % p99_latency)
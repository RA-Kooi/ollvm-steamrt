# Overview

This is a port of various LLVM obfuscators to a somewhat recent version of LLVM.  
The targeted version of LLVM is the same as the one in the current latest Steam runtime (version 4).  
The Steam runtime is not needed in any way whatsoever, I just decided that's the version I am targeting.

# Features

You can annotate functions to obfuscate specific functions instead of a whole translation unit,  
or to selectively disable obfuscations:
```cpp
void __attribute__((annotate("fla nobcf"))) Test()
{
    puts("Test");
}
```
The annotations match the flags passed to llvm driver.  
Prefixing the annotation with `no` disables that specific pass for that function.

## Original obfuscator-llvm features
- Bogus control flow `-mllvm -bcf`
  - `-mllvm bcf_prob=70` controls the probability of each basic block being processed by the pass.
  - `-mllvm bcf_loop=2` controls the amount of times the pass loops on a function.
- Basic block splitting `-mllvm -split`
  - `-mllvm -split_num=3` controls the amount of times each block is split.
- Instruction substitution `-mllvm -sub`
- Seeding of the PRNG `-mllvm aesSeed=3ec1795344a7f787454c09c755e215001`  
  The seed must be a 32 character long string of hexadecimal digits (leading 0x accepted).

## Hikari features
- Control flow flattening `-mllvm -fla`

## Goron features
- Indirect branching `-mllvm -ibr`
- Indirect function calls `-mllvm -icall`
- String obfuscation `-mllvm -sobf`
- Indirect global variables `-mllvm -igv`  
  Puts global variables in a table adding a layer of indirection.

# Credits
[Obfuscator](https://github.com/obfuscator-llvm/obfuscator) By the original obfuscator-llvm team  
[Hikari](https://github.com/HikariObfuscator/Core) By [Naville](https://github.com/Naville)  
[goron](https://github.com/amimo/goron) by amimo  

# Disclaimer

I claim no responsibility for any shortcomings, miscompilations, and bugs in this port.  
I futhermore am not responsible for any type of deployment of this port. Any and all  
responsibilty of (ab)use falls on the user of this software and not on me the porter.

#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/Orc/CompileUtils.h>
#include <llvm/ExecutionEngine/Orc/IRCompileLayer.h>
#include <llvm/ExecutionEngine/Orc/IRTransformLayer.h>
#include <llvm/ExecutionEngine/Orc/ObjectTransformLayer.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/TargetSelect.h>
#include <iostream>
#include <thread>

// Container for JIT-ed code. The generated code is very simple, we generate the equivalent of
// int foo(int(*bar)(int), int v) { return bar(v); }
// We just want to trigger the libgcc code path for JITed code and check if unwinding though
// generate code works
class JITContainer {
   private:
   struct JIT;

   using CallbackSignature = int (*)(int);
   using Signature = int (*)(CallbackSignature, int);
   std::unique_ptr<JIT> jit;
   Signature jitedCode;

   public:
   JITContainer();
   ~JITContainer();

   int invoke(CallbackSignature callback, int v) const { return jitedCode(callback, v); }
};

// The interface to LLVM
struct JITContainer::JIT {
   llvm::orc::ThreadSafeContext context;
   std::unique_ptr<llvm::TargetMachine> targetMachine;
   llvm::orc::ExecutionSession es;
   llvm::orc::RTDyldObjectLinkingLayer objectLayer;
   llvm::orc::ObjectTransformLayer objectTransformLayer;
   llvm::orc::IRCompileLayer compileLayer;
   llvm::orc::IRTransformLayer optimizeLayer;
   llvm::orc::JITDylib& mainDylib;

   JIT(std::unique_ptr<llvm::LLVMContext>&& context, std::unique_ptr<llvm::Module>&& module, llvm::EngineBuilder& builder)
      : context(move(context)),
        targetMachine(builder.selectTarget()),
        es(std::make_unique<llvm::orc::UnsupportedExecutorProcessControl>()),
        objectLayer(es, []() { return std::make_unique<llvm::SectionMemoryManager>(); }),
        objectTransformLayer(es, objectLayer),
        compileLayer(es, objectTransformLayer, std::make_unique<llvm::orc::SimpleCompiler>(*targetMachine)),
        optimizeLayer(es, compileLayer, [](llvm::orc::ThreadSafeModule m, const llvm::orc::MaterializationResponsibility&) { return m; }),
        mainDylib(cantFail(es.createJITDylib("exe"))) {
      llvm::cantFail(optimizeLayer.add(mainDylib, llvm::orc::ThreadSafeModule(move(module), this->context)));
   }
   ~JIT() { llvm::cantFail(es.endSession()); }
   void* dlsym(const char* name) {
      auto sym = es.lookup(&mainDylib, name);
      return (sym) ? reinterpret_cast<void*>(static_cast<uintptr_t>(sym->getAddress())) : nullptr;
   }
};

JITContainer::JITContainer() {
   // Generate the IR code for foo
   auto c = std::make_unique<llvm::LLVMContext>();
   auto m = std::make_unique<llvm::Module>("module", *c);
   auto it = llvm::Type::getInt32Ty(*c);
   llvm::Type* args1[1] = {it};
   auto ft1 = llvm::FunctionType::get(it, args1, false);
   llvm::Type* args2[2] = {ft1->getPointerTo(), it};
   auto ft2 = llvm::FunctionType::get(it, args2, false);
   auto f = llvm::Function::Create(ft2, llvm::Function::ExternalLinkage, "foo", &*m);
   {
      auto callback = f->getArg(0);
      auto v = f->getArg(1);
      auto b = llvm::BasicBlock::Create(*c, "body", f);
      llvm::IRBuilder<> builder(*c);
      builder.SetInsertPoint(b);
      llvm::Value* args[1] = {v};
      auto call = builder.CreateCall(ft1, callback, args);
      builder.CreateRet(call);
   }

   // Compile into machine code
   llvm::EngineBuilder engineBuilder;
   jit = std::make_unique<JIT>(move(c), move(m), engineBuilder);
   jitedCode = reinterpret_cast<Signature>(jit->dlsym("foo"));
}

JITContainer::~JITContainer() {
}

// The callback function that we use. Throws on input<1
static int callback(int v) {
   if (v < 1) throw v;
   if (v & 1) return 3 * v + 1;
   return v / 2;
}

// A helper function for tests. Checks that we get the expected output
static bool doTest(const JITContainer& jitCode, int input, int expected) {
   try {
      int r = jitCode.invoke(callback, input);
      if ((r < 0) || (r != expected)) {
         std::cerr << "unexpected result for input " << input << ", expected " << expected << ", got " << r << std::endl;
         exit(1);
      }
   } catch (int) {
      if (expected >= 0) {
         std::cerr << "unexpected result for input " << input << ", expected " << expected << ", got exception" << std::endl;
         exit(1);
      }
   }
   return true;
}

// Sanity test to check the generated code works as intended
static void sanityTest(const JITContainer& jitCode) {
   doTest(jitCode, 2, 1);
   doTest(jitCode, 1, 4);
   doTest(jitCode, 0, -1);
   doTest(jitCode, -1, -1);
}

// A weak but fast PRNG is good enough for this. Use xorshift.
// We seed it with the thread id to get deterministic behavior
struct Random {
   uint64_t state;
   Random(uint64_t seed) : state((seed << 1) | 1) {}

   uint64_t operator()() {
      uint64_t x = state;
      x ^= x >> 12;
      x ^= x << 25;
      x ^= x >> 27;
      state = x;
      return x * 0x2545F4914F6CDD1DULL;
   }
};

// One run with a certain error rate
static unsigned doTest(unsigned errorRate, unsigned seed) {
   Random random(seed);

   // Execute the function n times and measure the runtime
   auto start = std::chrono::steady_clock::now();
   constexpr unsigned functionRepeat = 10;
   constexpr unsigned repeat = 10000;
   unsigned result = 0;
   for (unsigned pass = 0; pass != functionRepeat; ++pass) {
      // We frequently generate new JIT code to put pressure on the JIT registration mechanism
      JITContainer jitCode;

      // Invoke the generated code repeatedly
      for (unsigned index = 0; index != repeat; ++index) {
         // Cause a failure with a certain probability
         auto r = random();
         int arg = ((r % 1000) < errorRate) ? -1 : ((r & 0xFFFF) + 1);
         int expected = (arg < 1) ? -1 : ((arg & 1) ? (3 * arg + 1) : (arg / 2));

         // Call the function itself
         result += doTest(jitCode, arg, expected);
      }
   }
   if (!result)
      std::cerr << "invalid result!" << std::endl;
   auto stop = std::chrono::steady_clock::now();

   return std::chrono::duration_cast<std::chrono::milliseconds>(stop - start).count();
};

// Perform the test using n threads
static unsigned doTestMultithreaded(unsigned errorRate, unsigned threadCount) {
   if (threadCount <= 1) return doTest(errorRate, 0);

   std::vector<std::thread> threads;
   std::atomic<unsigned> maxDuration{0};
   threads.reserve(threadCount);
   for (unsigned index = 0; index != threadCount; ++index) {
      threads.push_back(std::thread([index, errorRate, &maxDuration]() {
         unsigned duration = doTest(errorRate, index);
         unsigned current = maxDuration.load();
         while ((duration > current) && (!maxDuration.compare_exchange_weak(current, duration))) {}
      }));
   };
   for (auto& t : threads) t.join();
   return maxDuration.load();
}

// Test with different thread counts
static void runTests(const std::vector<unsigned>& threadCounts) {
   const unsigned failureRates[] = {0, 1, 10, 100};

   std::cout << "testing  using";
   for (auto c : threadCounts) std::cout << " " << c;
   std::cout << " threads" << std::endl;
   for (unsigned fr : failureRates) {
      std::cout << "failure rate " << (static_cast<double>(fr) / 10.0) << "%:";
      for (auto tc : threadCounts)
         std::cout << " " << doTestMultithreaded(fr, tc);
      std::cout << std::endl;
   }
}

static std::vector<unsigned> buildThreadCounts(unsigned maxCount) {
   std::vector<unsigned> threadCounts{1};
   while (threadCounts.back() < maxCount) threadCounts.push_back(std::min(threadCounts.back() * 2, maxCount));
   return threadCounts;
}

static std::vector<unsigned> interpretThreadCounts(std::string desc) {
   std::vector<unsigned> threadCounts;
   auto add = [&](const std::string& desc) {
      unsigned c = std::stoi(desc);
      if (c) threadCounts.push_back(c);
   };
   while (desc.find(' ') != std::string::npos) {
      auto split = desc.find(' ');
      add(desc.substr(0, split));
      desc = desc.substr(split + 1);
   }
   add(desc);
   return threadCounts;
}

int main(int argc, char* argv[]) {
   // Handle arguments
   std::vector<unsigned> threadCounts = buildThreadCounts(std::thread::hardware_concurrency() / 2); // assuming half are hyperthreads. We can override that below
   for (int index = 1; index < argc; ++index) {
      std::string o = argv[index];
      if ((o == "--threads") && (index + 1 < argc)) {
         threadCounts = interpretThreadCounts(argv[++index]);
      } else {
         std::cout << "unknown option " << o << std::endl;
         return 1;
      }
   }

   // Init llvm
   llvm::InitializeNativeTarget();
   llvm::InitializeNativeTargetAsmPrinter();

   // Sanity tests
   JITContainer container;
   sanityTest(container);

   // Multi-rhreaded tests
   runTests(threadCounts);
}

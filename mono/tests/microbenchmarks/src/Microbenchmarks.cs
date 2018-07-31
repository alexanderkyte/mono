using BenchmarkDotNet.Configs;
using BenchmarkDotNet.Jobs;
using BenchmarkDotNet.Running;
using BenchmarkDotNet.Environments;
using BenchmarkDotNet.Diagnosers;

namespace MonoAOTMicrobenchmark
{
	public class MonoAOTMicrobenchmarkConfig : ManualConfig
	{
		public MonoAOTMicrobenchmarkConfig ()
		{
			// --optimize=MODE , -O=mode
			// MODE is a comma separated list of optimizations. They also allow
			// optimizations to be turned off by prefixing the optimization
			// name with a minus sign.
			var mono_path = System.Environment.GetEnvironmentVariable ("MONO_EXECUTABLE");

			//Add(Job.ShortRun
			//.With(new MonoRuntime("Mono In-Tree", mono_path))
			//.With(new[] { new MonoArgument("--aot=llvm") })
			//.With(Jit.Llvm)
			//.WithId("AOT Phase"));

			Add(Job.ShortRun
			.With(new MonoRuntime("Mono In-Tree", mono_path))
			//.With(new[] { new MonoArgument("") })
			.With(Jit.Llvm)
			.WithId("AOT Run Phase"));

			Add(DisassemblyDiagnoser.Create(new DisassemblyDiagnoserConfig(printIL: true, printAsm: true, printPrologAndEpilog: true, recursiveDepth: 0)));
		}
	}
}

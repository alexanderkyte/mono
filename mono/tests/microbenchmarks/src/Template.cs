
/*
  The Computer Language Benchmarks Game
  https://salsa.debian.org/benchmarksgame-team/benchmarksgame/

  contributed by Marek Safar
  optimized by kasthack
  *reset*
*/

using BenchmarkDotNet.Attributes;
using BenchmarkDotNet.Configs;
using BenchmarkDotNet.Jobs;
using BenchmarkDotNet.Running;

using System;
using System.Threading;
using System.Threading.Tasks;

namespace MonoAOTMicrobenchmark
{
	[Config(typeof(MonoAOTMicrobenchmarkConfig))]
	public class Template
	{
		[Benchmark]
		public void Run ()
		{
		}
	}

	public class Runner {
		public static void Main (string [] args)
		{
			BenchmarkRunner.Run<Template>();
		}
	}
}



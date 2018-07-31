
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
	public  class BinaryTrees
	{
		const int MinDepth = 4;

	  [Params(18)]
		public int maxDepth;

		[Benchmark]
		public void Run ()
		{
			int stretchDepth = maxDepth + 1;

			var stretchTreeCheck = Task.Run(() => TreeNode.BottomUpTree(stretchDepth).ItemCheck());

			var longLivedTree = Task.Run(() =>
			{
				var tree = TreeNode.BottomUpTree(maxDepth);
				return Tuple.Create(tree.ItemCheck(), tree);
			});

			var results = new Task<string>[(maxDepth - MinDepth) / 2 + 1];

			for (int depth = MinDepth; depth <= maxDepth; depth += 2)
			{
				int iterations = 1 << (maxDepth - depth + MinDepth);

				int check = 0, safeDept = depth;

				results[(safeDept - MinDepth) / 2] = Task.Run(() =>
				{
					int i = 1;
					while (i <= iterations)
					{
						if (safeDept > 18)
						{
							var split = new[]
							{
								Task.Run(() => (TreeNode.BottomUpTree(safeDept)).ItemCheck()),
								Task.Run(() => (TreeNode.BottomUpTree(safeDept)).ItemCheck())
							};

							i += 2;
							Task.WaitAll(split);
							check += split[0].Result + split[1].Result;
						} else {
							check += (TreeNode.BottomUpTree(safeDept)).ItemCheck();
							i++;
						}
					}

					return $"{iterations}\t trees of depth {safeDept}\t check: {check}";
				});
			}

			Console.WriteLine("stretch tree of depth {0}\t check: {1}", stretchDepth, stretchTreeCheck.Result);

			for (int i = 0; i < results.Length; i++)
			{
				Console.WriteLine(results[i].Result);
			}

			Console.WriteLine("long lived tree of depth {0}\t check: {1}", maxDepth, longLivedTree.Result.Item1);
		}
	}


	struct TreeNode
	{
		class Next
		{
			public TreeNode left, right;
		}

		private Next next;

		internal static TreeNode BottomUpTree(int depth)
		{
			if (depth > 0) {
				return new TreeNode(BottomUpTree(depth - 1), BottomUpTree(depth - 1));
			} else {
				return new TreeNode();
			}
		}

		TreeNode(TreeNode left, TreeNode right)
		{
			next = new Next {
				left = left,
				right = right
			};
		}

		internal int ItemCheck()
		{
			if (next == null)
			{
				return 1;
			} else {
				return 1 + next.left.ItemCheck() + next.right.ItemCheck();
			}
		}
	}

	public class Runner {
		public static void Main (string [] args)
		{
			BenchmarkRunner.Run<BinaryTrees>();
		}
	}
}



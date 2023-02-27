# ParZagros
[![Generic badge](https://img.shields.io/badge/Version-0.0.1-blue.svg)](https://shields.io/)


ParZargos is a parallel application for motif discovery based on original application of [Zagros](https://github.com/smithlabcode/zagros) from Smith Lab, a computacional biology research group at USC (University of South California).

## Requirements
To be able to compile this application the following libraries must be installed:
+ build-essentials
+ libgsl-dev
+ libomp-dev
+ libopenmpi-dev
  
## Compilation
To compile the application simply run the makefile.

## Execution
First set the number of threads to use with the env variable `OMP_NUM_THREADS`. An example of a simple run would be:

`mpirun -np $NUM_PROCESSES bin/zagros -n $NUM_MOTIF -s $NUM_STARTING_POINTS $GENOME_PATH`

With the parameters as:
+ NUM_PROCESSES: Number of processes to use with MPI
+ NUM_MOTIF: Number of motifs to discover
+ STARTING_POINTS: Number of kmers to use for the discovery
+ GENOME_PATH: Path to a Fasta file with the sequences of the genome

For more parameters check the [readme](https://github.com/smithlabcode/zagros/blob/master/README.TXT) of the original application.

## License
ParZagros is distributed under the [GPL License](LICENSE). 
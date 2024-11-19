#!/bin/bash

echo "starts"

for i in {17..57}
do
	qdel "4786$i"
	#qsub -v INDEX="$i" ./projecctA/pbses/parser_pbs.sh
done

echo "finished"


for i in {5..8}; do
    # Filter lines starting with "[$i]" and containing either "EndSort", "StoreTerm", or "MergePatches"
    echo "=== Index $i ===" > index6_$i.txt
    grep "^\[$i\].*\(EndSort\|StoreTerm\|MergePatches\)" ./test_results/test10_6.txt >> index6_$i.txt
done

#!/bin/bash

set -euo pipefail

#required env var $GINAN to be set to the checkout root
export PATH="$PATH:$GINAN/bin"

S3_PREFIX=s3://peanpod/pea/long-tests/ex31/$(date --iso-8601=seconds)/

# ginan source tree is at /ginan
echo "*** Downloading examples ***"
cd $GINAN
python scripts/download_examples.py

cd $GINAN/examples
mkdir -p ex31 ex31/pod_fit ex31/pea ex31/pod_ic

echo "*** Run POD (fit) ***"
pod -y ex31_pod_fit_gps.yaml | tee pod.out
mv pod.out ex31/pod_fit

if aws s3 ls > /dev/null 2>&1; then aws s3 sync ex31/ $S3_PREFIX; fi

cd ex31/pod_fit
$GINAN/scripts/rms_bar_plot.py -i gag20624_igs20624_orbdiff_rtn.out -d . -c G >pod_G.rms
$GINAN/scripts/compare_pod_rms.py -ro pod.out -rr pod_G.rms -so ../../solutions/ex31/pod_fit/pod.out -sr ../../solutions/ex31/pod_fit/pod_G.rms -em 0.0002

if aws s3 ls > /dev/null 2>&1; then aws s3 sync ex31/ $S3_PREFIX; fi

cd $GINAN/examples
echo "*** Run PEA to update estimated parameters ***"
pea --config ex31_pea_pp_netw_gnss_orb_ar.yaml

if aws s3 ls > /dev/null 2>&1; then aws s3 sync ex31/ $S3_PREFIX; fi

python $GINAN/scripts/diffsnx.py -i ex31/pea/ex31.snx -o solutions/ex31/pea/ex31.snx
for trace in `ls ex31/pea/*.TRACE`; do
    tracebase="$(basename $trace)";
    python $GINAN/scripts/difftrace.py -i $trace -o "solutions/ex31/pea/$tracebase";
done

if aws s3 ls > /dev/null 2>&1; then aws s3 sync ex31/ $S3_PREFIX; fi

echo "*** rerun pod from the ic file generated by pea"
pod -y ex31_pod_ic_gps.yaml | tee pod.out
mv pod.out ex31/pod_ic
cd ex31/pod_ic
$GINAN/scripts/rms_bar_plot.py -i gag20624_igs20624_orbdiff_rtn.out -d . -c G >pod_G.rms
$GINAN/scripts/compare_pod_rms.py -ro pod.out -rr pod_G.rms -so ../../solutions/ex31/pod_icint/pod.out -sr ../../solutions/ex31/pod_icint/pod_G.rms -em 0.0002 

if aws s3 ls > /dev/null 2>&1; then aws s3 sync ex31/ $S3_PREFIX; fi
exit 0

# the following is old and does not work
# Store the result files in a S3 bucket
echo "*** Store Results in S3"
ls -1 /ginan/examples/ex31/pea/* | xargs -L1 -I% aws s3 cp % s3://peanpod/pea/long-tests/${bitbucket_branch}/network-orbit/


# copy the clock file for processing in PPP later
#cp /data/acs/pea/output/EX03/EX03.clk /data/acs/pea/proc/exs/products/gag20624.clk
# copy the smoothed clock file for processing in PPP later:
cp /data/acs/pea/output/EX03/EX03.clk_smoothed /data/acs/pea/proc/exs/products/gag20624.clk

#==============================================================================
# Create an SP3 file from the POD
# copy the config files needed by POD and the results files into a seperate work
# directory /data/acs/pea/output/ex03-check
#==============================================================================
mkdir -p /data/acs/pea/output/ex03-check && cd /data/acs/pea/output/ex03-check
# TODO: check which file in and where I am copying:
cp /data/acs/pea/proc/exs/products/igs20624.sp3 ./
cp /data/acs/pea/proc/exs/products/orb_partials/gag20624_orbits_partials.out.ecom2_pea ./
cp /data/acs/pea/aws/long-tests/config/POD.in ./
cp /data/acs/pea/aws/long-tests/config/EQM.in ./
cp /data/acs/pea/aws/long-tests/config/VEQ.in ./
cp /data/acs/pod/tables/* ./

echo "*** Running the POD"
echo `date`
# run the pod to produce the SP3 file based on the pod partials
pod
echo "*** Checking whether POD runs with this command"
# copy the sp3 file <gag20624.sp3> to s3 and for processing by the pea later
aws s3 cp gag20624.sp3 s3://peanpod/pea/long-tests/${bitbucket_branch}/network-orbit/
cp gag20624.sp3 /data/acs/pea/proc/exs/products/

echo "*** Copying to s3"
# copy the orbex back to s3 <gag20624.obx>
aws s3 cp gag20624.obx s3://peanpod/pea/long-tests/${bitbucket_branch}/network-orbit/

echo "*** Setting up conda"
# Set up the python environment for plotting
eval "$(command conda 'shell.bash' 'hook' 2> /dev/null)"
conda activate gn37

# =============================================================================
# run the plotting scripts ..
# =============================================================================
echo "*** Running plot scripts"
python3 /data/acs/pod/scripts/res_plot.py -i gag20624_igs20624_orbdiff_rtn.out -d . -c G
aws s3 cp gag20624_igs20624_orbdiff_rtn.out s3://peanpod/pea/long-tests/${bitbucket_branch}/network-orbit/
aws s3 cp orbres_gag20624_igs20624.out_G.png s3://peanpod/pea/long-tests/${bitbucket_branch}/network-orbit/

# copy the png file orbres_gag20624_igs20624.out_G.png to s3, or send via email
python3 /data/acs/pod/scripts/rms_bar_plot.py -i gag20624_igs20624_orbdiff_rtn.out -d . -c G >& pod.rms
aws s3 cp gag20624_igs20624_orbdiff_rtn.out s3://peanpod/pea/long-tests/${bitbucket_branch}/network-orbit/
aws s3 cp pod.rms s3://peanpod/pea/long-tests/${bitbucket_branch}/network-orbit/

# TODO: Should store the results from master and download them to here for comparison
#diff pod.out ./solution/pod.out
#diff pod.rms ./solution/pod.rms
#diff gag20624.sp3 ./solution/gag20624.sp3

echo "*** Running PEA - PPP solutions using the new orbit and clocks"
cd /data/acs/pea/aws/long-tests
./runLongTest_EX01_IF_PPP_PEA_PRODUCTS_SMOOTHED.sh

# copy results back to S3
cd /data/acs/pea/output/exs/LONG_EX01_IF_PEA_PRODUCTS_SMOOTHED/
ls -1 /data/acs/pea/output/exs/LONG_EX01_IF_PEA_PRODUCTS_SMOOTHED/* | xargs -L1 -I% aws s3 cp % s3://peanpod/pea/long-tests/${bitbucket_branch}/ppp-pea-products-smoothed/

# Compare the PPP solutions to the IGS solutions
ls -1 *.TRACE | xargs -L1 -I% python /data/acs/pea/python/source/trace_plot.py -y_lims /-0.1,0.1 -PPP_diff /data/acs/pea/proc/exs/products/igs19P2062.snx /data/acs/pea/output/exs/LONG_EX01_IF_PEA_PRODUCTS_SMOOTHED/% ./

echo "*** Running PEA - PPP solutions using the IGS orbit and clocks"
cd /data/acs/pea/aws/long-tests
./runLongTest_EX01_IF_PPP.sh

# copy results back to S3
cd /data/acs/pea/output/exs/LONG_EX01_IF_IGS_PRODUCTS/
ls -1 /data/acs/pea/output/exs/LONG_EX01_IF_IGS_PRODUCTS/* | xargs -L1 -I% aws s3 cp % s3://peanpod/pea/long-tests/${bitbucket_branch}/ppp-pea-products-smoothed/

# Compare the PPP solutions to the IGS solutions
ls -1 *.TRACE | xargs -L1 -I% python /data/acs/pea/python/source/trace_plot.py -y_lims /-0.1,0.1 -PPP_diff /data/acs/pea/proc/exs/products/igs19P2062.snx /data/acs/pea/output/exs/LONG_EX01_IF_PARALLEL_PEA/% ./

#==============================================================================
# Other tests to add
# 1) Single frequency PPP
# 2) Multi-gnss solutions
# 3) Ionosphere solutions
#==============================================================================
#echo "getting spot request name"
#echo $(aws ec2 describe-spot-instance-requests --filters Name=instance-id,Values="$(wget -q -O - http://169.254.169.254/latest/meta-data/instance-id)"   --region ap-southeast-2 |   jq '.SpotInstanceRequests[0].SpotInstanceRequestId' --raw-output)

#aws ec2 cancel-spot-instance-requests --spot-instance-request-ids $(aws ec2 describe-spot-instance-requests --filters Name=instance-id,Values="$(wget -q -O - http://169.254.169.254/latest/meta-data/instance-id)"   --region ap-southeast-2 |   jq '.SpotInstanceRequests[0].SpotInstanceRequestId' --raw-output) --region ap-southeast-2

echo "*** Publishing build log"
aws sns publish --topic-arn "arn:aws:sns:ap-southeast-2:604917042985:acs-release-testing" --message file:///data/build.log --region ap-southeast-2

#sudo shutdown -h now

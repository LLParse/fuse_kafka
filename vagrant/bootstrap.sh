#!/bin/bash -Eu

log_dir=/tmp
test_dir=$log_dir/fuse-kafka-test
topic=logs

# Execute with root permissions
[ "$UID" -eq 0 ] || exec sudo bash "$0" "$@"

# Install dependencies
apt-get -qy update
apt-get install -y build-essential curl git openjdk-7-jre libprotobuf-c0-dev protobuf-c-compiler \
	libssl-dev libzookeeper-mt-dev librdkafka-dev libfuse-dev libjansson-dev

# Build and start fuse_kafka
mkdir -p $log_dir $test_dir
/vagrant/build.py
/vagrant/fuse_kafka _ -oallow_other -ononempty -s -omodules=subdir,subdir=. -f -- \
	--tags test \
	--fields hostname `hostname` \
	--directories "$test_dir" \
	--quota 65536 \
	--topic "$topic" \
	--zookeepers `hostname`:2181 &> "$log_dir/fuse_kafka.log" &
sleep 20

# Start zookeeper & kafka
/vagrant/build.py zookeeper_start &> $test_dir/zookeeper.log &
kafka_dir=/vagrant/kafka_2.8.0-0.8.1.1
export KAFKA_LOG4J_OPTS="-Dlog4j.configuration=file:$kafka_dir/config/log4j.properties"
export KAFKA_HEAP_OPTS="-Xmx256M -Xms256M"
exec $kafka_dir/bin/kafka-run-class.sh -name kafkaServer -loggc kafka.Kafka \
	$kafka_dir/config/server.properties &> $test_dir/kafka.log &

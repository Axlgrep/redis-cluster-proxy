require 'redis'
require 'hiredis'

setup {
    use_valgrind = $options[:valgrind] == true
    loglevel = $options[:log_level] || 'debug'
    dump_queues = $options[:dump_queues]
    dump_queries = $options[:dump_queries]
    dump_buffer = $options[:dump_buffer]
    if !$main_cluster
        @cluster = RedisCluster.new
        @cluster.restart
        $main_cluster = @cluster
    end

    if !$main_proxy
        @proxy = RedisClusterProxy.new $main_cluster,
                                       log_level: 'debug',
                                       dump_queries: dump_queries,
                                       dump_queues: dump_queues,
                                       dump_buffer: dump_buffer,
                                       valgrind: use_valgrind
        @proxy.start
        $main_proxy = @proxy
    end

    @aux_proxy = RedisClusterProxy.new $main_cluster,
                                       log_level: 'debug',
                                       disable_multiplexing: 'always'
    @aux_proxy.start
}

cleanup {
    @aux_proxy.stop
    @aux_proxy = nil
}

$numkeys = 500
$numclients = $options[:clients] || 10
#$node_down_for = 4
if $options[:max_keys] && $numkeys > $options[:max_keys]
    $numkeys = $options[:max_keys]
end

require 'thread'

test "SET #{$numkeys} keys to test (No multiplexing)" do
    spawn_clients(1, proxy: @aux_proxy){|client, idx|
        (0...$numkeys).each{|n|
            val = n.to_s
            reply = redis_command client, :set, "k:#{n}", val
            assert_not_redis_err(reply)
        }
    }
end

test "GET #{$numkeys} keys (No multiplexing)" do
    spawn_clients($numclients, proxy: @aux_proxy){|client, idx|
        (0...$numkeys).each{|n|
            log_test_update "key #{n + 1}/#{$numkeys}"
            key = "k:#{n}"
            val = n.to_s
            reply = redis_command client, :get, key
            assert_not_redis_err(reply)
            assert_equal(reply, val)
        }
    }
end

test "GET/SET #{$numkeys} keys (Disable multiplexing on some clients)" do
    disabled = []
    spawn_clients(21, proxy: $main_proxy){|client, idx|
        do_disable = ((idx % 2) == 0)
        phase = [:before_set, :after_set, :after_get][idx % 3]
        after_keys = $numkeys * ([0.25, 0.5, 0.75][idx % 3])
        (0...$numkeys).each{|n|
            should_disable = false
            if do_disable && !disabled[idx] && n >= after_keys
                should_disable = true
                if phase == :before_set
                    reply = redis_command client, :proxy, :multiplexing, :off
                    assert_not_redis_err(reply)
                    assert_equal(reply, 'OK')
                    disabled[idx] = true
                end
            end
            log_test_update "key #{n + 1}/#{$numkeys}"
            key = "k:#{idx}:#{n}"
            val = n.to_s * 4096
            reply = redis_command client, :set, key, val
            assert_not_redis_err(reply)
            if should_disable && phase == :after_set
                reply = redis_command client, :proxy, :multiplexing, :off
                assert_not_redis_err(reply)
                assert_equal(reply, 'OK')
                disabled[idx] = true
            end
            reply = redis_command client, :get, key
            assert_not_redis_err(reply)
            assert_equal(reply, val)
            if should_disable && phase == :after_get
                reply = redis_command client, :proxy, :multiplexing, :off
                assert_not_redis_err(reply)
                assert_equal(reply, 'OK')
                disabled[idx] = true
            end
        }
    }
end

numkeys = $numkeys / 10
numkeys = 10 if numkeys < 10

test "SET #{numkeys} keys (clients=#{$numclients})" do
    spawn_clients($numclients, proxy: $main_proxy){|client, idx|
        (0...numkeys).each{|n|
            log_test_update "key #{n + 1}/#{numkeys}"
            val = n.to_s
            key = "k:#{n}"
            reply = redis_command client, :set, key, val
            assert_not_redis_err(reply)
        }
    }
end

test "GET #{numkeys} keys (clients=#{$numclients}, multiplex=off, pipeline)" do
    log_test_update ''
    STDOUT.flush
    spawn_clients($numclients, proxy: $main_proxy){|client, idx|
        expected = ['OK']
        keys = (0...numkeys).map{|n|
            key = "k:#{n}"
            val = n.to_s
            expected << val
            key
        }
        begin
            reply = client.pipelined{
                client.proxy 'multiplexing', 'off'
                keys.each{|k|
                    client.get k
                }
            }
        rescue Redis::CommandError => cmderr
            reply = cmderr
        end
        assert_not_redis_err(reply)
        assert_equal(expected, reply)
    }
end

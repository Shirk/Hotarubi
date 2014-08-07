require 'open-uri'
require 'openssl'
require 'uri'
require 'yaml'
require 'rake/clean'

TC_META  = YAML::load_file( "#{File.dirname( __FILE__ )}/toolchain-meta.yaml" )
TC_FLAGS = YAML::load_file( "#{File.dirname( __FILE__ )}/toolchain-flags.yaml" )

TC = {
  :cxx => 'toolchain/bin/x86_64-elf-g++',
  :cc  => 'toolchain/bin/x86_64-elf-gcc',
  :as  => 'toolchain/bin/x86_64-elf-as',
  :ld  => 'toolchain/bin/x86_64-elf-ld',
  :oc  => 'toolchain/bin/x86_64-elf-objcopy',
  :od  => 'toolchain/bin/x86_64-elf-objdump',
  :nm  => 'toolchain/bin/x86_64-elf-nm'
}
TC.merge!( YAML::load_file( 'toolchain.yaml' ) ) if File.exists?( 'toolchain.yaml' )

namespace :toolchain do
  desc "Build the host toolchain and required dependencies (defined in toolchain-meta.yaml)."
  task :build do
    puts "done building."
  end

  desc "Remove source archives, sources and build paths."
  task :clean do
    _CLEAN = FileList.new  << "toolchain/arc" << "toolchain/src" << "toolchain/bld"
    rm_r( _CLEAN, :force => true )
  end

  desc "Remove source archives, sources, build paths and the toolchain binaries."
  task :clobber do
    _CLOBBER = FileList.new << "toolchain"
    rm_r( _CLOBBER, :force => true )
  end

  case RUBY_PLATFORM
    when /darwin/
      so_suffix = '.dylib'
    when /linux/
      so_suffix = '.so'
    when /[Ww]indows/
      so_suffix = '.dll'
    else
      fail "Unsupported host platform '#{RUBY_PLATFORM}'!"
  end

  # auto generate tasks
  TC_META[ :archives ].each do |target, meta|
    # setup source downloads
    source_arch = "toolchain/arc/#{File.basename( meta[ :uri ] )}"
    source_path = "toolchain/src/#{File.basename( meta[ :uri ], meta[ :uri ].gsub( /^.*(\.tar.\.*)/, '\1' ))}"
    build_path  = "toolchain/bld/#{File.basename( source_path )}"

    file source_arch do
      makedirs "toolchain/arc"
      begin
        length = 0
        URI.parse( meta[ :uri ] ).open( {
          :content_length_proc => lambda { |size| length = size },
          :progress_proc       => lambda { |size| print ".. fetching #{source_arch} [#{size / 1024}/#{length / 1024}kb]...    \r" },
          :read_timeout        => 60,
          :redirect            => true } ) do |io|

          File.open( source_arch, 'wb' ) { |f| f.write( io.read ); puts }
        end

        unless length == meta[ :size ]
          fail "Size mismatch for #{source_arch} (expected #{meta[ :size ]} got #{length})!"
        end

        if meta.has_key? :md5
          hash = OpenSSL::Digest::MD5.new
          hash.update( File.read( source_arch ) )
          fail "MD5 mismatch for #{source_arch}!" if hash.hexdigest != meta[ :md5 ]
        end

        if meta.has_key? :sha
          hash = OpenSSL::Digest::SHA256.new
          hash.update( File.read( source_arch ) )
          fail "SHA256 mismatch for #{source_arch}!" if hash.hexdigest != meta[ :sha ]
        end

      rescue SystemCallError => err
          fail "Error while loading #{meta[ :uri ]}: #{err.to_s}"
      end
    end

    # setup archive extraction
    file source_path => source_arch do
      makedirs "toolchain/src"
      tar_flags = case File.extname( meta[ :uri ] )
        when '.gz'   then '-xzf'
        when '.tgz'  then '-xzf'
        when '.bz2'  then '-xjf'
        when '.tbz2' then '-xjf'
        else '-xf'
      end

      sh "tar #{tar_flags} #{source_arch} -C toolchain/src/"
      # apply patches as needed
      patches = meta[ :patches ] ||= []
      Dir.chdir( source_path ) do
        patches.each do |patch|
          sh "patch -p1 < ../../../scripts/patches/#{patch}"
        end
      end
    end

    # setup the build target
    deps = [ source_path ] + ( meta[ :deps ] ||= [] )

    file "toolchain/.build-#{target}" => deps do
      unless File.exists? "toolchain/.build-#{target}"
        ( conf_flags = [ "--prefix=@PREFIX@" ] + ( meta[ :config_flags ] ||= [] ) ).map! do |flag|
          flag.gsub( /@PREFIX@/, "#{Dir.pwd}/toolchain/" )
        end

        makedirs build_path
        Dir.chdir( build_path ) do
          sh "../../src/#{File.basename( source_path )}/configure #{conf_flags.join( ' ' )}"
          unless meta.has_key? :targets
            sh "make && make install"
          else
            # build only specific targets
            meta[ :targets ].each do |target|
              sh "make #{target}"
            end
          end
        end
      end
      touch "toolchain/.build-#{target}"
    end

    #desc "Build #{target}"
    task target => "toolchain/.build-#{target}"

    unless meta.has_key? :optional
      task :build => target
    end
  end
end

# Rake helper tasks

[ '.c', '.cc', '.S' ].each do |source_ext|
  rule '.d' => [ source_ext ] do |t|
    include_path = "-I #{File.dirname( t.source )} #{TC_FLAGS[ :INCLUDE ].flatten.join( ' ' )}"

    File.open( t.name, "w") do |depfile|
      depfile.write( `#{TC[ :cxx ]} #{include_path} -MM #{t.source}`.gsub( /^(.*\.o:)/, t.source.ext( '.o:' ) ) )
    end
  end
end

# Rake helper functions

def cc_compile( source, target, **options )
  use32 = options.delete( :use32 ) || false

  include_paths = [ "-I #{File.dirname( source )}" ]
  if File.extname( source ) =~ /\.(cpp|hpp)/
    compiler      = TC[ :cxx ]
    compile_flags = include_paths + TC_FLAGS[ ( use32 ? :CXXFLAGS_32 : :CXXFLAGS_64 ) ].flatten
  else
    compiler      = TC[ :cc ]
    compile_flags = include_paths + TC_FLAGS[ ( use32 ? :CFLAGS_32 : :CFLAGS_64 ) ].flatten
  end

  puts "CC  #{use32 ? '[32]' : '    '} #{target}"
  sh "#{compiler} #{compile_flags.join( ' ' )} -c -o #{use32 ? target.ext( '.o32' ) : target} #{source}", :verbose => false

  if use32
    sh "#{TC[ :oc ]} -I elf32-i386 -O elf64-x86-64 #{target.ext( '.o32' )} #{target}", :verbose => false
    rm target.ext( '.o32' ), :verbose => false
  end
end

def cc_link( sources, target, **options )
  # provide some defaults
  link_flags = TC_FLAGS[ :LDFLAGS ].flatten.join( ' ' )
  defaults = {
    :crtbegin => '',
    :crtend   => '',
    :sources  => ''
  }

  # check for crti / crte and create the required linker context for them
  Array.new( sources ).each do |source|
    if File.basename( source ) == 'crti.o'
      defaults[ :crtbegin ] = "#{source} #{`#{TC[ :cc ]} #{link_flags} -print-file-name=crtbegin.o`.chomp}"
      sources.delete( source )
    elsif File.basename( source ) == 'crtn.o'
      defaults[ :crtend ] = "-lgcc #{`#{TC[ :cc ]} #{link_flags} -print-file-name=crtend.o`.chomp} #{source}"
      sources.delete( source )
    end
  end
  defaults[ :sources ] = sources.join( ' ' )

  puts "LD       #{target}"
  if defaults[ :crtbegin ].empty? and defaults[ :crtend ].empty?
    # normal link, no crt
    sh "#{TC[ :cc ]} -o #{target} #{link_flags} #{sources}", :verbose => false
  else
    # link against crt / libgcc, this requires a special order
    sh "#{TC[ :cc ]} -o #{target} #{link_flags} #{defaults[ :crtbegin ]} #{defaults[ :sources ]} #{defaults[ :crtend ]}", :verbose => false
  end
end

def cc_64to32( source, target, **options )
  cleanup = options.delete( :cleanup ) || false

  puts "OC  [32] #{target}"
  sh "#{TC[ :oc ]} -I elf64-x86-64 -O elf32-i386 #{source} #{target}", :verbose => false
  if cleanup
    rm source, :verbose => false
  end
end

#include <chrono>
#include<iostream>
#include <libtorrent/units.hpp>
#include<thread>
#include <libtorrent/add_torrent_params.hpp>
#include<libtorrent/version.hpp>
#include<libtorrent/session.hpp>
#include<libtorrent/magnet_uri.hpp>
#include<libtorrent/alert.hpp>
#include<libtorrent/settings_pack.hpp>
#include<libtorrent/torrent_status.hpp>
#include <libtorrent/torrent_info.hpp>
#include <string>
#include<filesystem>
namespace lt = libtorrent;
namespace fs =std::filesystem;
int main()
	{
	lt::settings_pack setting;
	setting.set_bool(lt::settings_pack::enable_dht,true);
	setting.set_bool(lt::settings_pack::enable_lsd,true);
	setting.set_bool(lt::settings_pack::enable_upnp,true);
	setting.set_bool(lt::settings_pack::enable_natpmp,true);
	  std::cout<< "Libtorrent version :"<< LIBTORRENT_VERSION<<"\n"<<std::endl;
	  lt::session ses(setting);
	  //This create a object session of Bittorrent engine
	  /*
	   Thing session as object of
	   |
	   |__Internet Networking
	   |__DHT
	   |__Tracker
	   |__peerConnection
	   |__torrent
	    
	   */
	  std::string magnet;
	  std::cout<<"Enter your magnet link here \t:";
	  std::getline(std::cin,magnet);
	  lt::add_torrent_params params= lt::parse_magnet_uri(magnet);
	  //params.save_path = "/test";
	fs::path download_path=fs::current_path() / "test";
	fs::create_directories(download_path);
	params.save_path=download_path.string();
	  lt::torrent_handle handle =ses.add_torrent(params);
	  std::cout<<"LIBTORRENT SESSION STARTED And Torrent addded";
	  /*
	   Main concept is get a magnet link than parse it to get meta data than add a 
	   torrent in the session of that param
	   */
	  /*
	   *TO GET IMP DATA WHILE DOWNLOADING WE NEED ALERTS
	   * */

	  /*Get Files to show*/
	 auto torrent_info = handle.torrent_file();

	auto const& files = torrent_info->files();

	std::cout << "\nFiles in torrent:\n";

	for (int i = 0; i < files.num_files(); ++i)
	{
    		auto file_index = lt::file_index_t{i};

    		std::cout << "[" << i << "]\t"
              << files.file_path(file_index)
              << " -- "
              << files.file_size(file_index) / (1024.0 * 1024.0)
              << " MB\n";
	}
	  std::string empty;
	  std::getline(std::cin,empty);
	 while(1)
	 {
		 std::vector<lt::alert*>alerts;
		 ses.pop_alerts(&alerts);
		 for(auto alert: alerts)
		 {
			 std::cout<<alert->message()<<"\n";
		 }
		 auto status = handle.status();

std::cout << "\nProgress: "
          << status.progress * 100 << "%";

std::cout << "\nDownload speed: "
          << status.download_rate / 1024.0 << " KB/s";

std::cout << "\nPeers: "
          << status.num_peers;

std::cout << "\nSeeds: "
          << status.num_seeds;

std::cout << "\n----------------------\n";
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	 }
	  return 0;
	}

// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.ui.platform;

import android.os.Bundle;
import android.util.TypedValue;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;
import androidx.recyclerview.widget.GridLayoutManager;
import androidx.recyclerview.widget.RecyclerView;
import androidx.swiperefreshlayout.widget.SwipeRefreshLayout;

import org.dolphinemu.dolphinemu.R;
import org.dolphinemu.dolphinemu.adapters.GameAdapter;
import org.dolphinemu.dolphinemu.databinding.FragmentGridBinding;
import org.dolphinemu.dolphinemu.services.GameFileCacheManager;

public final class PlatformGamesFragment extends Fragment implements PlatformGamesView
{
  private static final String ARG_PLATFORM = "platform";

  private GameAdapter mAdapter;
  private SwipeRefreshLayout mSwipeRefresh;
  private SwipeRefreshLayout.OnRefreshListener mOnRefreshListener;

  private FragmentGridBinding mBinding;

  public static PlatformGamesFragment newInstance(Platform platform)
  {
    PlatformGamesFragment fragment = new PlatformGamesFragment();

    Bundle args = new Bundle();
    args.putSerializable(ARG_PLATFORM, platform);

    fragment.setArguments(args);
    return fragment;
  }

  @Override
  public void onCreate(Bundle savedInstanceState)
  {
    super.onCreate(savedInstanceState);
  }

  @NonNull
  @Override
  public View onCreateView(@NonNull LayoutInflater inflater, ViewGroup container,
          Bundle savedInstanceState)
  {
    mBinding = FragmentGridBinding.inflate(inflater, container, false);
    return mBinding.getRoot();
  }

  @Override
  public void onViewCreated(@NonNull View view, Bundle savedInstanceState)
  {
    mSwipeRefresh = mBinding.swipeRefresh;

    int columns = getResources().getInteger(R.integer.game_grid_columns);
    RecyclerView.LayoutManager layoutManager = new GridLayoutManager(getActivity(), columns);
    mAdapter = new GameAdapter(requireActivity());

    TypedValue typedValue = new TypedValue();
    requireActivity().getTheme().resolveAttribute(R.attr.colorPrimary, typedValue, true);
    mSwipeRefresh.setColorSchemeColors(typedValue.data);

    mSwipeRefresh.setOnRefreshListener(mOnRefreshListener);

    mBinding.gridGames.setLayoutManager(layoutManager);
    mBinding.gridGames.setAdapter(mAdapter);

    mBinding.gridGames.addItemDecoration(new GameAdapter.SpacesItemDecoration(4));

    setRefreshing(GameFileCacheManager.isLoadingOrRescanning());

    showGames();
  }

  @Override
  public void onDestroyView()
  {
    super.onDestroyView();
    mBinding = null;
  }

  @Override
  public void refreshScreenshotAtPosition(int position)
  {
    mAdapter.notifyItemChanged(position);
  }

  @Override
  public void onItemClick(String gameId)
  {
    // No-op for now
  }

  @Override
  public void showGames()
  {
    if (mAdapter != null)
    {
      Platform platform = (Platform) getArguments().getSerializable(ARG_PLATFORM);
      mAdapter.swapDataSet(GameFileCacheManager.getGameFilesForPlatform(platform));
    }
  }

  @Override
  public void refetchMetadata()
  {
    mAdapter.refetchMetadata();
  }

  public void setOnRefreshListener(@Nullable SwipeRefreshLayout.OnRefreshListener listener)
  {
    mOnRefreshListener = listener;

    if (mSwipeRefresh != null)
    {
      mSwipeRefresh.setOnRefreshListener(listener);
    }
  }

  public void setRefreshing(boolean refreshing)
  {
    mBinding.swipeRefresh.setRefreshing(refreshing);
  }
}
